#include "smcommon.h"
#include <fstream>
#include <map>
#include <csignal>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dirent.h>
#endif

static SpoolIPC g_ipc;
static bool g_running = true;

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
    log_msg("Spooler shutting down gracefully...");
    cleanup_spool_ipc(g_ipc, true);
    cleanup_networking();
    std::exit(0);
}

// Ensure directory exists
bool ensure_dir(const std::string &path) {
#ifdef _WIN32
    if (_mkdir(path.c_str()) == 0 || errno == EEXIST) {
        return true;
    }
#else
    if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) {
        return true;
    }
#endif
    return false;
}

// User registry map: username -> UserRecord
std::map<std::string, UserRecord> g_users;
// Tracking monotonic next ID per user
std::map<std::string, uint64_t> g_user_next_id;

bool load_users(const std::string &filename) {
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << get_timestamp() << " Error: Cannot open userfile: " << filename << std::endl;
        return false;
    }

    std::string line;
    int line_num = 0;
    while (std::getline(infile, line)) {
        line_num++;
        // Remove trailing \r or spaces
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty()) continue; // Skip empty lines

        std::istringstream iss(line);
        std::string uname, pass, extra;
        if (!(iss >> uname >> pass) || (iss >> extra)) {
            std::cerr << get_timestamp() << " Error: Malformed userfile at line " << line_num << std::endl;
            return false;
        }

        std::string lower_u = to_lower(uname);
        if (!is_valid_username(lower_u)) {
            std::cerr << get_timestamp() << " Error: Invalid username '" << uname << "' at line " << line_num 
                      << " (must be 1-20 lowercase alphabetic chars)" << std::endl;
            return false;
        }

        if (!is_valid_password(pass)) {
            std::cerr << get_timestamp() << " Error: Invalid password for user '" << uname << "' at line " << line_num 
                      << " (must be 1-30 alphanumeric chars)" << std::endl;
            return false;
        }

        UserRecord rec;
        rec.username = lower_u;
        rec.password = pass;
        g_users[lower_u] = rec;
    }

    if (g_users.empty()) {
        std::cerr << get_timestamp() << " Error: Userfile contained no valid users." << std::endl;
        return false;
    }

    return true;
}

// Initialize mailbox directories and scan for next available monotonic ID per user
void init_user_mailboxes() {
    ensure_dir("mailboxes");

    for (const auto &pair : g_users) {
        const std::string &u = pair.first;
        std::string udir = "mailboxes/" + u;
        ensure_dir(udir);

        uint64_t max_id = 0;

        // Check if .next_id persistent tracking file exists
        std::string id_file = udir + "/.next_id";
        std::ifstream fin(id_file);
        if (fin.is_open()) {
            fin >> max_id;
            fin.close();
        }

        // Also scan directory to ensure max_id is at least > any existing file
#ifdef _WIN32
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA((udir + "/*.txt").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::string fname = fd.cFileName;
                size_t dot = fname.find(".txt");
                if (dot != std::string::npos) {
                    try {
                        uint64_t fid = std::stoull(fname.substr(0, dot));
                        if (fid >= max_id) {
                            max_id = fid + 1;
                        }
                    } catch (...) {}
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
#else
        DIR *dir = opendir(udir.c_str());
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                std::string fname = entry->d_name;
                size_t dot = fname.find(".txt");
                if (dot != std::string::npos) {
                    try {
                        uint64_t fid = std::stoull(fname.substr(0, dot));
                        if (fid >= max_id) {
                            max_id = fid + 1;
                        }
                    } catch (...) {}
                }
            }
            closedir(dir);
        }
#endif

        if (max_id < 1) max_id = 1;
        g_user_next_id[u] = max_id;

        // Persist initial next_id
        std::ofstream fout(id_file);
        if (fout.is_open()) {
            fout << max_id << std::endl;
            fout.close();
        }
    }
}

// Deliver a dequeued message to a recipient's mailbox
void deliver_message(const MessageSlot &slot, const std::string &recipient) {
    std::string lower_rec = to_lower(recipient);
    std::string udir = "mailboxes/" + lower_rec;
    ensure_dir(udir);

    uint64_t cur_id = g_user_next_id[lower_rec];
    if (cur_id < 1) cur_id = 1;
    g_user_next_id[lower_rec] = cur_id + 1;

    // Update .next_id file
    std::string id_file = udir + "/.next_id";
    std::ofstream fout_id(id_file);
    if (fout_id.is_open()) {
        fout_id << (cur_id + 1) << std::endl;
        fout_id.close();
    }

    std::string mail_filepath = udir + "/" + std::to_string(cur_id) + ".txt";
    std::ofstream mail_out(mail_filepath);
    if (!mail_out.is_open()) {
        std::cerr << get_timestamp() << " Error writing mail file: " << mail_filepath << std::endl;
        return;
    }

    // Comma-separated recipient list
    std::string recip_str;
    for (int i = 0; i < slot.recipient_count; ++i) {
        if (i > 0) recip_str += ", ";
        recip_str += slot.recipients[i];
    }

    // Required exact format:
    // From: <display_name>
    // To: <comma-separated list of accepted recipients>
    // Subject: <subject_text>
    // Date: <YYYY-MM-DD HH:MM:SS>
    // ---
    // <body text>
    mail_out << "From: " << slot.sender_display_name << "\n";
    mail_out << "To: " << recip_str << "\n";
    mail_out << "Subject: " << slot.subject << "\n";
    mail_out << "Date: " << (slot.date_str[0] ? slot.date_str : get_datetime_str()) << "\n";
    mail_out << "---\n";
    mail_out.write(slot.body, slot.body_len);
    // Ensure trailing newline if needed
    if (slot.body_len == 0 || slot.body[slot.body_len - 1] != '\n') {
        mail_out << "\n";
    }
    mail_out.close();

    log_msg("Mail spooled to disk: ticket " + std::to_string(slot.ticket_id) + 
            " -> " + mail_filepath);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <userfile>" << std::endl;
        return 1;
    }

    init_networking();
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string userfile = argv[1];
    if (!load_users(userfile)) {
        cleanup_networking();
        return 1;
    }

    init_user_mailboxes();

    log_msg("Initializing SimpleMail background spooler daemon (smspooler)...");

    if (!init_spool_shm_creator(g_ipc)) {
        std::cerr << get_timestamp() << " Error: Failed to initialize shared memory spool ring." << std::endl;
        cleanup_networking();
        return 1;
    }

    log_msg("Spooler initialized for " + std::to_string(g_users.size()) + 
            " users. Waiting for messages in SM_SPOOL ring buffer...");

    // Consumer loop: wait on full_slots, dequeue, write mail files, signal empty_slots
    while (g_running) {
        spool_wait_full(g_ipc);
        if (!g_running) break;

        spool_lock_mutex(g_ipc);
        int tail = g_ipc.ring->tail;
        MessageSlot slot = g_ipc.ring->slots[tail];
        g_ipc.ring->tail = (tail + 1) % RING_BUFFER_CAPACITY;
        spool_unlock_mutex(g_ipc);

        // Signal empty slot available in shared ring buffer
        spool_post_empty(g_ipc);

        log_msg("Dequeued mail ticket " + std::to_string(slot.ticket_id) + 
                " from " + slot.sender_display_name + " for " + 
                std::to_string(slot.recipient_count) + " recipient(s)");

        // Deliver to each accepted recipient's mailbox
        for (int i = 0; i < slot.recipient_count; ++i) {
            deliver_message(slot, slot.recipients[i]);
        }
    }

    cleanup_spool_ipc(g_ipc, true);
    cleanup_networking();
    return 0;
}
