#include "smcommon.h"
#include <fstream>
#include <map>
#include <vector>
#include <csignal>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dirent.h>
    #include <sys/time.h>
#endif

// Client connection state machine
enum ClientProtocolState {
    STATE_AWAIT_MODE,          // Just connected, sent greeting, awaiting MODE SEND or MODE RECV
    STATE_SMTP2_AWAIT_FROM,    // SMTP2: awaiting FROM <display_name>
    STATE_SMTP2_AWAIT_TO,      // SMTP2: received FROM, awaiting TO <username> or SUB <subject>
    STATE_SMTP2_AWAIT_BODY,    // SMTP2: received SUB, awaiting BODY_STREAM or BODY
    STATE_SMTP2_IN_BODY_STREAM,// SMTP2: receiving CHUNKs with credit windowing
    STATE_SMTP2_IN_BODY_LEGACY,// SMTP2: receiving lines terminated by CRLF.CRLF
    STATE_SMP_AWAIT_AUTH,      // SMP: sent challenge nonce, awaiting AUTH <user> <hash>
    STATE_SMP_CMD              // SMP: authenticated, awaiting LIST, READ, DELETE, COUNT, QUIT
};

struct ClientSession {
    socket_t sock;
    std::string ip;
    int port;
    std::time_t connect_time;
    ClientProtocolState state;
    std::string recv_buf;

    // SMTP2 State
    std::string smtp_from;
    std::vector<std::string> smtp_recipients;
    std::string smtp_subject;
    std::string smtp_body;
    int window_credits;
    size_t chunk_expected_bytes;
    bool in_chunk_payload;

    // SMP State
    std::string smp_username;
    std::string current_nonce;
    int auth_failures;

    ClientSession() : sock(INVALID_SOCK), port(0), connect_time(0),
                      state(STATE_AWAIT_MODE), window_credits(DEFAULT_WINDOW_SIZE),
                      chunk_expected_bytes(0), in_chunk_payload(false),
                      auth_failures(0) {}
};

static SpoolIPC g_ipc;
static socket_t g_listen_sock = INVALID_SOCK;
static bool g_running = true;
static uint64_t g_ticket_counter = 0;
static std::map<std::string, UserRecord> g_users;
static std::map<socket_t, ClientSession> g_clients;

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
    log_msg("Server shutting down gracefully...");
    if (g_listen_sock != INVALID_SOCK) {
        close_socket(g_listen_sock);
        g_listen_sock = INVALID_SOCK;
    }
    for (auto &pair : g_clients) {
        close_socket(pair.first);
    }
    g_clients.clear();
    cleanup_spool_ipc(g_ipc, false);
    cleanup_networking();
    std::exit(0);
}

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
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (line.empty()) continue;

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

// Send data to client
bool send_str(socket_t sock, const std::string &str) {
    const char *p = str.c_str();
    size_t total = str.length();
    while (total > 0) {
        int n = send(sock, p, (int)total, 0);
        if (n <= 0) return false;
        p += n;
        total -= n;
    }
    return true;
}

// Enqueue message into shared memory spool ring
bool enqueue_message_to_spool(const ClientSession &client, uint64_t &ticket_id) {
    spool_wait_empty(g_ipc);
    spool_lock_mutex(g_ipc);

    int head = g_ipc.ring->head;
    MessageSlot &slot = g_ipc.ring->slots[head];
    memset(&slot, 0, sizeof(MessageSlot));

    ticket_id = ++g_ticket_counter;
    slot.ticket_id = ticket_id;

    strncpy(slot.sender_display_name, client.smtp_from.c_str(), MAX_DISPLAY_NAME_LEN);
    slot.recipient_count = (int)std::min(client.smtp_recipients.size(), (size_t)MAX_RECIPIENTS);
    for (int i = 0; i < slot.recipient_count; ++i) {
        strncpy(slot.recipients[i], client.smtp_recipients[i].c_str(), MAX_USERNAME_LEN);
    }
    strncpy(slot.subject, client.smtp_subject.c_str(), MAX_SUBJECT_LEN);

    size_t b_len = std::min(client.smtp_body.length(), (size_t)MAX_BODY_SIZE);
    memcpy(slot.body, client.smtp_body.data(), b_len);
    slot.body_len = b_len;

    std::string now_str = get_datetime_str();
    strncpy(slot.date_str, now_str.c_str(), sizeof(slot.date_str) - 1);

    g_ipc.ring->head = (head + 1) % RING_BUFFER_CAPACITY;

    spool_unlock_mutex(g_ipc);
    spool_post_full(g_ipc);

    return true;
}

// Helper: Close and clean up client session
void disconnect_client(socket_t s, const std::string &reason) {
    if (g_clients.find(s) != g_clients.end()) {
        const auto &cl = g_clients[s];
        std::string ident = cl.smp_username.empty() ? (cl.ip + ":" + std::to_string(cl.port)) : cl.smp_username;
        log_msg("Client " + ident + " disconnected: " + reason);
        close_socket(s);
        g_clients.erase(s);
    }
}

// Parse email file headers
bool parse_email_headers(const std::string &filepath, std::string &from, std::string &subject, std::string &date) {
    std::ifstream fin(filepath);
    if (!fin.is_open()) return false;

    from = "";
    subject = "";
    date = "";

    std::string line;
    while (std::getline(fin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line == "---") break;

        if (line.rfind("From: ", 0) == 0) {
            from = line.substr(6);
        } else if (line.rfind("Subject: ", 0) == 0) {
            subject = line.substr(9);
        } else if (line.rfind("Date: ", 0) == 0) {
            date = line.substr(6);
        }
    }
    return true;
}

// Handle client incoming data
void process_client_data(socket_t s) {
    ClientSession &cl = g_clients[s];

    // If waiting for raw chunk bytes in BODY_STREAM mode
    if (cl.state == STATE_SMTP2_IN_BODY_STREAM && cl.in_chunk_payload) {
        size_t needed = cl.chunk_expected_bytes;
        size_t available = cl.recv_buf.length();

        if (available >= needed) {
            // Extract raw chunk bytes
            std::string chunk_data = cl.recv_buf.substr(0, needed);
            cl.recv_buf.erase(0, needed);
            cl.in_chunk_payload = false;
            cl.chunk_expected_bytes = 0;

            if (cl.smtp_body.length() + chunk_data.length() > MAX_BODY_SIZE) {
                send_str(s, "ERR Body too large\r\n");
                disconnect_client(s, "Body size exceeded 65536 bytes limit");
                return;
            }

            cl.smtp_body.append(chunk_data);

            // Deduct available credit window
            cl.window_credits -= (int)chunk_data.length();
            if (cl.window_credits < 0) cl.window_credits = 0;

            // Reply with remaining window credits
            send_str(s, "OK Window " + std::to_string(cl.window_credits) + "\r\n");

            // Flow control: As server consumes data, provide credit replenishments if window is low
            if (cl.window_credits < 512) {
                int replenishment = 2048;
                cl.window_credits += replenishment;
                send_str(s, "WINDOW_UPDATE " + std::to_string(replenishment) + "\r\n");
            }
        } else {
            // Wait for more data to complete the chunk
            return;
        }
    }

    // Process line-oriented commands
    while (true) {
        // If we switched to raw chunk in the middle of loop, stop line processing
        if (cl.state == STATE_SMTP2_IN_BODY_STREAM && cl.in_chunk_payload) {
            if (cl.recv_buf.length() >= cl.chunk_expected_bytes) {
                // Next iteration of outer loop will handle it
                break;
            } else {
                return;
            }
        }

        size_t crlf_pos = cl.recv_buf.find("\r\n");
        if (crlf_pos == std::string::npos) {
            // Check max command line length limit (512 bytes including CRLF)
            if (cl.recv_buf.length() > MAX_CMD_LEN) {
                send_str(s, "ERR Command line too long\r\n");
                disconnect_client(s, "Command line length exceeded 512 bytes");
            }
            break;
        }

        std::string line = cl.recv_buf.substr(0, crlf_pos);
        cl.recv_buf.erase(0, crlf_pos + 2); // Remove line and \r\n

        // Handle states
        if (cl.state == STATE_AWAIT_MODE) {
            if (line == "MODE SEND") {
                cl.state = STATE_SMTP2_AWAIT_FROM;
                send_str(s, "OK\r\n");
                log_msg("Client " + cl.ip + ":" + std::to_string(cl.port) + " selected MODE SEND");
            } else if (line == "MODE RECV") {
                cl.state = STATE_SMP_AWAIT_AUTH;
                cl.current_nonce = generate_nonce();
                cl.auth_failures = 0;
                send_str(s, "OK\r\n");
                send_str(s, "AUTH REQUIRED " + cl.current_nonce + "\r\n");
                log_msg("Client " + cl.ip + ":" + std::to_string(cl.port) + " selected MODE RECV (challenge issued)");
            } else {
                send_str(s, "ERR Unknown mode\r\n");
            }
        }
        else if (cl.state == STATE_SMP_AWAIT_AUTH) {
            if (line.rfind("AUTH ", 0) == 0) {
                std::istringstream iss(line.substr(5));
                std::string uname;
                std::string hash_str;
                bool parsed_ok = false;
                unsigned long long client_hash = 0;
                if (iss >> uname >> hash_str) {
                    try {
                        client_hash = std::stoull(hash_str);
                        parsed_ok = true;
                    } catch (...) {
                        parsed_ok = false;
                    }
                }

                if (parsed_ok) {
                    std::string lower_u = to_lower(uname);
                    bool auth_ok = false;
                    if (g_users.find(lower_u) != g_users.end()) {
                        const std::string &pwd = g_users[lower_u].password;
                        std::string challenge_str = pwd + cl.current_nonce;
                        uint64_t exp64 = djb2_64(challenge_str.c_str());
                        uint32_t exp32 = djb2_32(challenge_str.c_str());
                        if (client_hash == exp64 || client_hash == exp32 || (uint32_t)client_hash == exp32) {
                            auth_ok = true;
                        }
                    }

                    if (auth_ok) {
                        cl.smp_username = lower_u;
                        cl.state = STATE_SMP_CMD;
                        send_str(s, "OK Welcome " + lower_u + "\r\n");
                        log_msg("Authentication success for user " + lower_u);
                    } else {
                        cl.auth_failures++;
                        log_msg("Authentication failure for user '" + uname + "' (attempt " + 
                                std::to_string(cl.auth_failures) + "/3)");
                        if (cl.auth_failures >= 3) {
                            send_str(s, "ERR Too many failures\r\n");
                            disconnect_client(s, "Too many authentication failures");
                            return;
                        } else {
                            cl.current_nonce = generate_nonce();
                            send_str(s, "ERR Authentication failed\r\n");
                            send_str(s, "AUTH REQUIRED " + cl.current_nonce + "\r\n");
                        }
                    }
                } else {
                    cl.auth_failures++;
                    if (cl.auth_failures >= 3) {
                        send_str(s, "ERR Too many failures\r\n");
                        disconnect_client(s, "Too many authentication failures");
                        return;
                    } else {
                        cl.current_nonce = generate_nonce();
                        send_str(s, "ERR Authentication failed\r\n");
                        send_str(s, "AUTH REQUIRED " + cl.current_nonce + "\r\n");
                    }
                }
            } else if (line == "QUIT") {
                send_str(s, "BYE\r\n");
                disconnect_client(s, "Client issued QUIT");
                return;
            } else {
                cl.auth_failures++;
                if (cl.auth_failures >= 3) {
                    send_str(s, "ERR Too many failures\r\n");
                    disconnect_client(s, "Too many authentication failures");
                    return;
                } else {
                    cl.current_nonce = generate_nonce();
                    send_str(s, "ERR Authentication failed\r\n");
                    send_str(s, "AUTH REQUIRED " + cl.current_nonce + "\r\n");
                }
            }
        }
        else if (cl.state == STATE_SMP_CMD) {
            if (line == "QUIT") {
                send_str(s, "BYE\r\n");
                disconnect_client(s, "Client issued QUIT");
                return;
            } else if (line == "COUNT") {
                std::string udir = "mailboxes/" + cl.smp_username;
                int count = 0;
#ifdef _WIN32
                WIN32_FIND_DATAA fd;
                HANDLE hFind = FindFirstFileA((udir + "/*.txt").c_str(), &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        count++;
                    } while (FindNextFileA(hFind, &fd));
                    FindClose(hFind);
                }
#else
                DIR *dir = opendir(udir.c_str());
                if (dir) {
                    struct dirent *entry;
                    while ((entry = readdir(dir)) != NULL) {
                        std::string fname = entry->d_name;
                        if (fname.find(".txt") != std::string::npos) count++;
                    }
                    closedir(dir);
                }
#endif
                send_str(s, "OK " + std::to_string(count) + "\r\n");
                log_msg("Mailbox access (COUNT: " + std::to_string(count) + ") for user " + cl.smp_username);
            } else if (line == "LIST") {
                std::string udir = "mailboxes/" + cl.smp_username;
                struct MailItem {
                    uint64_t id;
                    std::string from;
                    std::string subject;
                    std::string date;
                };
                std::vector<MailItem> items;

#ifdef _WIN32
                WIN32_FIND_DATAA fd;
                HANDLE hFind = FindFirstFileA((udir + "/*.txt").c_str(), &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        std::string fname = fd.cFileName;
                        size_t dot = fname.find(".txt");
                        if (dot != std::string::npos) {
                            try {
                                uint64_t id = std::stoull(fname.substr(0, dot));
                                std::string fpath = udir + "/" + fname;
                                std::string from, sub, date;
                                parse_email_headers(fpath, from, sub, date);
                                items.push_back({id, from, sub, date});
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
                                uint64_t id = std::stoull(fname.substr(0, dot));
                                std::string fpath = udir + "/" + fname;
                                std::string from, sub, date;
                                parse_email_headers(fpath, from, sub, date);
                                items.push_back({id, from, sub, date});
                            } catch (...) {}
                        }
                    }
                    closedir(dir);
                }
#endif
                std::sort(items.begin(), items.end(), [](const MailItem &a, const MailItem &b) {
                    return a.id < b.id;
                });

                send_str(s, "OK " + std::to_string(items.size()) + " messages\r\n");
                for (const auto &item : items) {
                    // <id>\t<from_display_name>\t<subject>\t<date>\r\n
                    std::string entry = std::to_string(item.id) + "\t" + item.from + "\t" + 
                                        item.subject + "\t" + item.date + "\r\n";
                    send_str(s, entry);
                }
                send_str(s, ".\r\n");
                log_msg("Mailbox access (LIST: " + std::to_string(items.size()) + " messages) for user " + cl.smp_username);
            } else if (line.rfind("READ ", 0) == 0) {
                std::string id_str = line.substr(5);
                while (!id_str.empty() && (id_str.front() == ' ' || id_str.front() == '\t')) id_str.erase(0, 1);
                std::string fpath = "mailboxes/" + cl.smp_username + "/" + id_str + ".txt";

                std::ifstream mail_in(fpath);
                if (!mail_in.is_open()) {
                    send_str(s, "ERR No such message\r\n");
                } else {
                    send_str(s, "OK\r\n");
                    std::string mline;
                    bool in_body = false;
                    while (std::getline(mail_in, mline)) {
                        while (!mline.empty() && (mline.back() == '\r' || mline.back() == '\n')) mline.pop_back();

                        if (!in_body) {
                            if (mline == "---") {
                                in_body = true;
                            }
                            send_str(s, mline + "\r\n");
                        } else {
                            // Dot-stuffing for body lines starting with '.'
                            if (!mline.empty() && mline[0] == '.') {
                                send_str(s, "." + mline + "\r\n");
                            } else {
                                send_str(s, mline + "\r\n");
                            }
                        }
                    }
                    send_str(s, ".\r\n");
                    log_msg("Mailbox access (READ message " + id_str + ") for user " + cl.smp_username);
                }
            } else if (line.rfind("DELETE ", 0) == 0) {
                std::string id_str = line.substr(7);
                while (!id_str.empty() && (id_str.front() == ' ' || id_str.front() == '\t')) id_str.erase(0, 1);
                std::string fpath = "mailboxes/" + cl.smp_username + "/" + id_str + ".txt";

                if (std::remove(fpath.c_str()) == 0) {
                    send_str(s, "OK Deleted\r\n");
                    log_msg("Mailbox access (DELETE message " + id_str + ") for user " + cl.smp_username);
                } else {
                    send_str(s, "ERR No such message\r\n");
                }
            } else {
                send_str(s, "ERR Unknown command\r\n");
            }
        }
        else if (cl.state == STATE_SMTP2_AWAIT_FROM) {
            if (line == "QUIT") {
                send_str(s, "BYE\r\n");
                disconnect_client(s, "Client issued QUIT");
                return;
            } else if (line.rfind("FROM ", 0) == 0) {
                std::string dname = line.substr(5);
                while (!dname.empty() && (dname.front() == ' ' || dname.front() == '\t')) dname.erase(0, 1);
                while (!dname.empty() && (dname.back() == ' ' || dname.back() == '\t')) dname.pop_back();

                if (dname.empty()) {
                    send_str(s, "ERR Invalid sender\r\n");
                } else {
                    cl.smtp_from = dname;
                    cl.smtp_recipients.clear();
                    cl.smtp_subject = "";
                    cl.smtp_body = "";
                    cl.state = STATE_SMTP2_AWAIT_TO;
                    send_str(s, "OK Sender accepted.\r\n");
                }
            } else {
                send_str(s, "ERR Bad sequence\r\n");
            }
        }
        else if (cl.state == STATE_SMTP2_AWAIT_TO) {
            if (line == "QUIT") {
                send_str(s, "BYE\r\n");
                disconnect_client(s, "Client issued QUIT");
                return;
            } else if (line.rfind("TO ", 0) == 0) {
                std::string uname = line.substr(3);
                while (!uname.empty() && (uname.front() == ' ' || uname.front() == '\t')) uname.erase(0, 1);
                while (!uname.empty() && (uname.back() == ' ' || uname.back() == '\t')) uname.pop_back();

                std::string lower_u = to_lower(uname);
                if (g_users.find(lower_u) != g_users.end()) {
                    // Check duplicate
                    if (std::find(cl.smtp_recipients.begin(), cl.smtp_recipients.end(), lower_u) == cl.smtp_recipients.end()) {
                        cl.smtp_recipients.push_back(lower_u);
                    }
                    send_str(s, "OK Recipient accepted.\r\n");
                } else {
                    send_str(s, "ERR No such user\r\n");
                }
            } else if (line.rfind("SUB", 0) == 0) {
                // Cannot proceed to SUB if no recipient accepted
                if (cl.smtp_recipients.empty()) {
                    send_str(s, "ERR Bad sequence\r\n");
                } else {
                    std::string sub = "";
                    if (line.length() >= 4 && line[3] == ' ') {
                        sub = line.substr(4);
                        while (!sub.empty() && (sub.front() == ' ' || sub.front() == '\t')) sub.erase(0, 1);
                    }
                    if (sub.empty()) {
                        sub = "(no subject)";
                    }
                    cl.smtp_subject = sub;
                    cl.state = STATE_SMTP2_AWAIT_BODY;
                    send_str(s, "OK Subject accepted.\r\n");
                }
            } else {
                send_str(s, "ERR Bad sequence\r\n");
            }
        }
        else if (cl.state == STATE_SMTP2_AWAIT_BODY) {
            if (line == "QUIT") {
                send_str(s, "BYE\r\n");
                disconnect_client(s, "Client issued QUIT");
                return;
            } else if (line == "BODY_STREAM") {
                cl.state = STATE_SMTP2_IN_BODY_STREAM;
                cl.window_credits = DEFAULT_WINDOW_SIZE;
                cl.smtp_body = "";
                cl.in_chunk_payload = false;
                cl.chunk_expected_bytes = 0;
                send_str(s, "OK Window " + std::to_string(cl.window_credits) + "\r\n");
            } else if (line == "BODY") {
                cl.state = STATE_SMTP2_IN_BODY_LEGACY;
                cl.smtp_body = "";
                send_str(s, "OK Send body, end with CRLF.CRLF.\r\n");
            } else {
                send_str(s, "ERR Bad sequence\r\n");
            }
        }
        else if (cl.state == STATE_SMTP2_IN_BODY_STREAM) {
            if (line == "END_BODY") {
                // Finalize and enqueue message
                uint64_t ticket = 0;
                enqueue_message_to_spool(cl, ticket);

                std::string ok_resp = "OK Queued " + std::to_string(ticket) + " to " + 
                                     std::to_string(cl.smtp_recipients.size()) + " mailboxes\r\n";
                send_str(s, ok_resp);
                log_msg("Mail queued to shared memory: ticket " + std::to_string(ticket) + 
                        ", from '" + cl.smtp_from + "', to " + 
                        std::to_string(cl.smtp_recipients.size()) + " mailboxes");

                // Return to await another FROM or QUIT
                cl.state = STATE_SMTP2_AWAIT_FROM;
            } else if (line.rfind("CHUNK ", 0) == 0) {
                // Zero-Window Stall & Backpressure Check
                if (cl.window_credits <= 0) {
                    send_str(s, "ERR Window overflow\r\n");
                    disconnect_client(s, "Client transmitted when credit window was 0 (Window overflow)");
                    return;
                }

                try {
                    size_t count = std::stoul(line.substr(6));
                    if ((int)count > cl.window_credits) {
                        send_str(s, "ERR Window overflow\r\n");
                        disconnect_client(s, "Chunk size exceeds available credit window (Window overflow)");
                        return;
                    }

                    cl.chunk_expected_bytes = count;
                    cl.in_chunk_payload = true;

                    // If recv_buf already contains the chunk payload, process immediately
                    if (cl.recv_buf.length() >= count) {
                        std::string chunk_data = cl.recv_buf.substr(0, count);
                        cl.recv_buf.erase(0, count);
                        cl.in_chunk_payload = false;
                        cl.chunk_expected_bytes = 0;

                        if (cl.smtp_body.length() + chunk_data.length() > MAX_BODY_SIZE) {
                            send_str(s, "ERR Body too large\r\n");
                            disconnect_client(s, "Body size exceeded 65536 bytes limit");
                            return;
                        }

                        cl.smtp_body.append(chunk_data);
                        cl.window_credits -= (int)count;
                        if (cl.window_credits < 0) cl.window_credits = 0;

                        send_str(s, "OK Window " + std::to_string(cl.window_credits) + "\r\n");

                        // Replenish credits
                        if (cl.window_credits < 512) {
                            int replenishment = 2048;
                            cl.window_credits += replenishment;
                            send_str(s, "WINDOW_UPDATE " + std::to_string(replenishment) + "\r\n");
                        }
                    } else {
                        // Will finish when more bytes arrive
                        return;
                    }
                } catch (...) {
                    send_str(s, "ERR Invalid chunk size\r\n");
                }
            } else {
                send_str(s, "ERR Bad sequence\r\n");
            }
        }
        else if (cl.state == STATE_SMTP2_IN_BODY_LEGACY) {
            if (line == ".") {
                // End of body
                uint64_t ticket = 0;
                enqueue_message_to_spool(cl, ticket);

                std::string ok_resp = "OK Queued " + std::to_string(ticket) + " to " + 
                                     std::to_string(cl.smtp_recipients.size()) + " mailboxes\r\n";
                send_str(s, ok_resp);
                log_msg("Mail queued to shared memory: ticket " + std::to_string(ticket) + 
                        ", from '" + cl.smtp_from + "', to " + 
                        std::to_string(cl.smtp_recipients.size()) + " mailboxes");

                cl.state = STATE_SMTP2_AWAIT_FROM;
            } else {
                // Byte-unstuffing: if line starts with '..', strip first '.'
                if (line.rfind("..", 0) == 0) {
                    line.erase(0, 1);
                }

                if (cl.smtp_body.length() + line.length() + 1 > MAX_BODY_SIZE) {
                    send_str(s, "ERR Body too large\r\n");
                    disconnect_client(s, "Body size exceeded 65536 bytes limit");
                    return;
                }
                cl.smtp_body.append(line + "\n");
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <userfile>" << std::endl;
        return 1;
    }

    int port = std::atoi(argv[1]);
    std::string userfile = argv[2];

    init_networking();
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (!load_users(userfile)) {
        cleanup_networking();
        return 1;
    }

    log_msg("Starting SimpleMail frontend network server (smserver)...");

    // Connect to shared memory created by smspooler
    if (!init_spool_shm_client(g_ipc)) {
        std::cerr << get_timestamp() << " Error: Cannot connect to shared memory spool ring (SM_SPOOL)." 
                  << " Please ensure smspooler is running first." << std::endl;
        cleanup_networking();
        return 1;
    }

    log_msg("Successfully connected to SM_SPOOL shared memory ring buffer.");

    g_listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_sock == INVALID_SOCK) {
        std::cerr << get_timestamp() << " Error creating socket." << std::endl;
        cleanup_spool_ipc(g_ipc, false);
        cleanup_networking();
        return 1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
#else
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons((uint16_t)port);

    if (bind(g_listen_sock, (sockaddr *)&saddr, sizeof(saddr)) == SOCK_ERR) {
        std::cerr << get_timestamp() << " Error: Could not bind to port " << port << std::endl;
        close_socket(g_listen_sock);
        cleanup_spool_ipc(g_ipc, false);
        cleanup_networking();
        return 1;
    }

    if (listen(g_listen_sock, 128) == SOCK_ERR) {
        std::cerr << get_timestamp() << " Error on listen." << std::endl;
        close_socket(g_listen_sock);
        cleanup_spool_ipc(g_ipc, false);
        cleanup_networking();
        return 1;
    }

    log_msg("smserver listening on port " + std::to_string(port) + 
            " (handling multiple clients via select)");

    // Concurrent event loop with select()
    char buffer[4096];
    while (g_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(g_listen_sock, &read_fds);

        socket_t max_fd = g_listen_sock;
        for (const auto &pair : g_clients) {
            FD_SET(pair.first, &read_fds);
            if (pair.first > max_fd) {
                max_fd = pair.first;
            }
        }

        struct timeval tv;
        tv.tv_sec = 1; // 1-second timeout for select to check connection timeouts
        tv.tv_usec = 0;

        int activity = select((int)(max_fd + 1), &read_fds, NULL, NULL, &tv);
        if (activity < 0) {
            if (!g_running) break;
            continue;
        }

        std::time_t now = std::time(NULL);

        // Check 30-second timeout for mode declaration
        std::vector<socket_t> timed_out_clients;
        for (const auto &pair : g_clients) {
            if (pair.second.state == STATE_AWAIT_MODE) {
                if (now - pair.second.connect_time >= 30) {
                    timed_out_clients.push_back(pair.first);
                }
            }
        }
        for (socket_t s : timed_out_clients) {
            disconnect_client(s, "30-second timeout exceeded without MODE declaration");
        }

        // New client connection
        if (FD_ISSET(g_listen_sock, &read_fds)) {
            sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            socket_t newsock = accept(g_listen_sock, (sockaddr *)&client_addr, &addrlen);
            if (newsock != INVALID_SOCK) {
                char *ip_str = inet_ntoa(client_addr.sin_addr);
                std::string ip_buf = ip_str ? ip_str : "unknown";
                int client_port = ntohs(client_addr.sin_port);

                ClientSession cl;
                cl.sock = newsock;
                cl.ip = ip_buf;
                cl.port = client_port;
                cl.connect_time = now;
                cl.state = STATE_AWAIT_MODE;
                g_clients[newsock] = cl;

                log_msg("New connection from " + cl.ip + ":" + std::to_string(cl.port));

                // Send greeting line
                send_str(newsock, "WELCOME SimpleMail v1.0\r\n");
            }
        }

        // Check activity on connected client sockets
        std::vector<socket_t> active_sockets;
        for (const auto &pair : g_clients) {
            if (FD_ISSET(pair.first, &read_fds)) {
                active_sockets.push_back(pair.first);
            }
        }

        for (socket_t s : active_sockets) {
            int n = recv(s, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                disconnect_client(s, "Client closed connection");
            } else {
                g_clients[s].recv_buf.append(buffer, n);
                process_client_data(s);
            }
        }
    }

    if (g_listen_sock != INVALID_SOCK) {
        close_socket(g_listen_sock);
    }
    for (auto &pair : g_clients) {
        close_socket(pair.first);
    }
    cleanup_spool_ipc(g_ipc, false);
    cleanup_networking();
    return 0;
}
