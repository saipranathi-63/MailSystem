#include "smcommon.h"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>

class SimpleMailClient {
private:
    std::string server_ip;
    int server_port;
    socket_t sock;
    std::string recv_buffer;

    bool connect_to_server() {
        if (sock != INVALID_SOCK) {
            close_socket(sock);
            sock = INVALID_SOCK;
        }
        recv_buffer.clear();

        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCK) {
            std::cerr << "Error creating socket." << std::endl;
            return false;
        }

        sockaddr_in saddr;
        memset(&saddr, 0, sizeof(saddr));
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons((uint16_t)server_port);
#ifdef _WIN32
        saddr.sin_addr.s_addr = inet_addr(server_ip.c_str());
        if (saddr.sin_addr.s_addr == INADDR_NONE) {
            std::cerr << "Invalid server IP address: " << server_ip << std::endl;
            close_socket(sock);
            sock = INVALID_SOCK;
            return false;
        }
#else
        if (inet_pton(AF_INET, server_ip.c_str(), &saddr.sin_addr) <= 0) {
            std::cerr << "Invalid server IP address: " << server_ip << std::endl;
            close_socket(sock);
            sock = INVALID_SOCK;
            return false;
        }
#endif

        if (connect(sock, (sockaddr *)&saddr, sizeof(saddr)) == SOCK_ERR) {
            std::cerr << "Failed to connect to server at " << server_ip << ":" << server_port << std::endl;
            close_socket(sock);
            sock = INVALID_SOCK;
            return false;
        }

        // Read server greeting: WELCOME SimpleMail v1.0
        std::string greeting = read_line();
        if (greeting.find("WELCOME") == std::string::npos) {
            std::cerr << "Unexpected server greeting: " << greeting << std::endl;
            close_socket(sock);
            sock = INVALID_SOCK;
            return false;
        }

        return true;
    }

    void disconnect() {
        if (sock != INVALID_SOCK) {
            send_command("QUIT");
            std::string bye = read_line();
            close_socket(sock);
            sock = INVALID_SOCK;
        }
        recv_buffer.clear();
    }

    bool send_raw(const std::string &data) {
        if (sock == INVALID_SOCK) return false;
        const char *p = data.c_str();
        size_t total = data.length();
        while (total > 0) {
            int n = send(sock, p, (int)total, 0);
            if (n <= 0) return false;
            p += n;
            total -= n;
        }
        return true;
    }

    bool send_command(const std::string &cmd) {
        return send_raw(cmd + "\r\n");
    }

    std::string read_line() {
        while (true) {
            size_t crlf = recv_buffer.find("\r\n");
            if (crlf != std::string::npos) {
                std::string line = recv_buffer.substr(0, crlf);
                recv_buffer.erase(0, crlf + 2);
                return line;
            }

            char buf[1024];
            int n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) {
                // Connection closed or error
                std::string line = recv_buffer;
                recv_buffer.clear();
                return line;
            }
            recv_buffer.append(buf, n);
        }
    }

public:
    SimpleMailClient(const std::string &ip, int port)
        : server_ip(ip), server_port(port), sock(INVALID_SOCK) {}

    ~SimpleMailClient() {
        disconnect();
    }

    bool start() {
        if (!connect_to_server()) {
            return false;
        }
        std::cout << "Connected to SimpleMail server." << std::endl;
        return true;
    }

    void handle_send_mail() {
        if (sock == INVALID_SOCK) {
            if (!connect_to_server()) return;
        }

        // Declare MODE SEND
        send_command("MODE SEND");
        std::string mode_resp = read_line();
        if (mode_resp != "OK") {
            std::cout << "[Server Error] " << mode_resp << std::endl;
            return;
        }

        // Prompt sender display name
        std::string display_name;
        while (display_name.empty()) {
            std::cout << "Enter sender display name: ";
            if (!std::getline(std::cin, display_name)) return;
            while (!display_name.empty() && (display_name.front() == ' ' || display_name.front() == '\t')) display_name.erase(0, 1);
            while (!display_name.empty() && (display_name.back() == ' ' || display_name.back() == '\t')) display_name.pop_back();
        }

        send_command("FROM " + display_name);
        std::string from_resp = read_line();
        if (from_resp.rfind("OK", 0) != 0) {
            std::cout << "[Server Error] " << from_resp << std::endl;
            return;
        }

        // Prompt recipient usernames
        std::vector<std::string> accepted_recipients;
        std::cout << "Enter recipient usernames (one per line, press Enter on empty line to finish):" << std::endl;
        while (true) {
            std::cout << "  Recipient: ";
            std::string rec;
            if (!std::getline(std::cin, rec)) return;
            while (!rec.empty() && (rec.front() == ' ' || rec.front() == '\t')) rec.erase(0, 1);
            while (!rec.empty() && (rec.back() == ' ' || rec.back() == '\t')) rec.pop_back();

            if (rec.empty()) {
                if (accepted_recipients.empty()) {
                    std::cout << "  [Notice] At least one valid recipient must be added before proceeding." << std::endl;
                    continue;
                } else {
                    break;
                }
            }

            send_command("TO " + rec);
            std::string to_resp = read_line();
            if (to_resp.rfind("OK", 0) == 0) {
                std::cout << "  [Server] OK Recipient '" << rec << "' accepted." << std::endl;
                accepted_recipients.push_back(to_lower(rec));
            } else {
                std::cout << "  [Server Error] " << to_resp << " for '" << rec << "'" << std::endl;
            }
        }

        // Prompt Subject
        std::cout << "Subject: ";
        std::string subject;
        if (!std::getline(std::cin, subject)) return;
        send_command("SUB " + subject);
        std::string sub_resp = read_line();
        if (sub_resp.rfind("OK", 0) != 0) {
            std::cout << "[Server Error] " << sub_resp << std::endl;
            return;
        }

        // Transmission Mode Choice
        std::cout << "\nSelect Transmission Mode:" << std::endl;
        std::cout << "1. BODY_STREAM (Credit-based Flow Control Windowing - Recommended)" << std::endl;
        std::cout << "2. BODY (Legacy dot-terminated CRLF.CRLF)" << std::endl;
        std::cout << "Choice [1/2]: ";
        std::string mode_choice;
        std::getline(std::cin, mode_choice);

        std::cout << "Enter email body text (type 'EOF' or a single '.' on a new line to finish):" << std::endl;
        std::string full_body;
        std::string bline;
        while (std::getline(std::cin, bline)) {
            if (bline == "EOF" || bline == ".") break;
            full_body.append(bline + "\n");
        }

        if (mode_choice == "2") {
            // Legacy mode
            send_command("BODY");
            std::string b_resp = read_line();
            std::cout << "[Server] " << b_resp << std::endl;

            std::istringstream iss(full_body);
            std::string line;
            while (std::getline(iss, line)) {
                // Byte-stuffing: leading '.' prepended if a line starts with '.'
                if (!line.empty() && line[0] == '.') {
                    send_command("." + line);
                } else {
                    send_command(line);
                }
            }
            send_command(".");
            std::string confirm = read_line();
            std::cout << "[Server Confirmation] " << confirm << std::endl;
        } else {
            // Flow Control Streaming Mode (BODY_STREAM)
            send_command("BODY_STREAM");
            std::string stream_resp = read_line();
            std::cout << "[Server] " << stream_resp << std::endl;

            int current_credit = 0;
            if (stream_resp.rfind("OK Window ", 0) == 0) {
                current_credit = std::stoi(stream_resp.substr(10));
            }

            // Chunk and transmit body with flow control credit monitoring
            const size_t CHUNK_SIZE = 512;
            size_t offset = 0;
            size_t total_bytes = full_body.length();

            while (offset < total_bytes) {
                // Check if we need to wait for credit window
                while (current_credit <= 0) {
                    std::cout << "[Flow Control] Window credits exhausted (0). Waiting for WINDOW_UPDATE..." << std::endl;
                    std::string upd = read_line();
                    std::cout << "[Server] " << upd << std::endl;
                    if (upd.rfind("WINDOW_UPDATE ", 0) == 0) {
                        int add_credits = std::stoi(upd.substr(14));
                        current_credit += add_credits;
                        std::cout << "[Flow Control] Received credit replenishment: +" << add_credits 
                                  << " bytes (Total: " << current_credit << ")" << std::endl;
                        break;
                    }
                }

                size_t to_send = std::min((size_t)CHUNK_SIZE, (size_t)current_credit);
                to_send = std::min(to_send, total_bytes - offset);

                std::string chunk_data = full_body.substr(offset, to_send);
                std::cout << "[Progress] Sending CHUNK " << to_send << " bytes (" 
                          << (offset + to_send) << "/" << total_bytes << " total)..." << std::endl;

                send_raw("CHUNK " + std::to_string(to_send) + "\r\n" + chunk_data);
                offset += to_send;

                // Server responds with updated window
                std::string win_resp = read_line();
                std::cout << "  -> Server response: " << win_resp << std::endl;

                if (win_resp.rfind("OK Window ", 0) == 0) {
                    current_credit = std::stoi(win_resp.substr(10));
                }

                // Check for unsolicited WINDOW_UPDATE frame
                if (!recv_buffer.empty() || (win_resp.rfind("WINDOW_UPDATE", 0) == 0)) {
                    if (win_resp.rfind("WINDOW_UPDATE ", 0) == 0) {
                        int add_credits = std::stoi(win_resp.substr(14));
                        current_credit += add_credits;
                        std::cout << "[Flow Control] Server sent WINDOW_UPDATE +" << add_credits << " bytes" << std::endl;
                    }
                }
            }

            // Signal completion with END_BODY
            send_command("END_BODY");
            std::string confirm = read_line();
            std::cout << "\n[Server Confirmation] " << confirm << std::endl;
        }

        // Close session or send QUIT so clean reconnect can happen
        disconnect();
    }

    void handle_check_mailbox() {
        if (sock == INVALID_SOCK) {
            if (!connect_to_server()) return;
        }

        std::cout << "\n--- Mailbox Authentication ---" << std::endl;
        std::cout << "Username: ";
        std::string uname;
        if (!std::getline(std::cin, uname)) return;
        while (!uname.empty() && (uname.front() == ' ' || uname.front() == '\t')) uname.erase(0, 1);
        while (!uname.empty() && (uname.back() == ' ' || uname.back() == '\t')) uname.pop_back();

        std::cout << "Password: ";
        std::string pass;
        if (!std::getline(std::cin, pass)) return;
        while (!pass.empty() && (pass.front() == ' ' || pass.front() == '\t')) pass.erase(0, 1);
        while (!pass.empty() && (pass.back() == ' ' || pass.back() == '\t')) pass.pop_back();

        // Declare MODE RECV
        send_command("MODE RECV");
        std::string mode_resp = read_line();
        if (mode_resp != "OK") {
            std::cout << "[Server Error] " << mode_resp << std::endl;
            return;
        }

        // Server sends: AUTH REQUIRED <nonce>
        std::string challenge = read_line();
        if (challenge.rfind("AUTH REQUIRED ", 0) != 0) {
            std::cout << "[Server Error] Expected AUTH REQUIRED challenge, got: " << challenge << std::endl;
            return;
        }

        std::string nonce = challenge.substr(14);
        while (!nonce.empty() && (nonce.back() == ' ' || nonce.back() == '\r')) nonce.pop_back();

        // Calculate DJB2 hash of password + nonce (no separator)
        std::string token = pass + nonce;
        unsigned long hash_val = djb2(token.c_str());

        // Send: AUTH <username> <hash>
        send_command("AUTH " + uname + " " + std::to_string(hash_val));
        std::string auth_resp = read_line();
        if (auth_resp.rfind("OK Welcome", 0) != 0) {
            std::cout << "[Authentication Failed] " << auth_resp << std::endl;
            disconnect();
            return;
        }

        std::cout << "[Server] " << auth_resp << "\n" << std::endl;

        // Mailbox Sub-menu
        while (true) {
            std::cout << "========================================" << std::endl;
            std::cout << " Mailbox Menu: " << to_lower(uname) << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "1. List emails (LIST)" << std::endl;
            std::cout << "2. Read an email (READ <id>)" << std::endl;
            std::cout << "3. Delete an email (DELETE <id>)" << std::endl;
            std::cout << "4. Mail count (COUNT)" << std::endl;
            std::cout << "5. Logout and Return to Main Menu" << std::endl;
            std::cout << "> ";

            std::string subchoice;
            if (!std::getline(std::cin, subchoice)) break;

            if (subchoice == "1") {
                send_command("LIST");
                std::string list_resp = read_line();
                std::cout << "\n[Server] " << list_resp << std::endl;
                if (list_resp.rfind("OK", 0) == 0) {
                    std::cout << "--------------------------------------------------------------------------------" << std::endl;
                    std::cout << std::left << std::setw(6) << "ID" 
                              << std::setw(20) << "From" 
                              << std::setw(30) << "Subject" 
                              << "Date" << std::endl;
                    std::cout << "--------------------------------------------------------------------------------" << std::endl;

                    while (true) {
                        std::string entry = read_line();
                        if (entry == "." || entry.empty()) break;

                        std::istringstream entry_iss(entry);
                        std::string id, from, subj, date;
                        std::getline(entry_iss, id, '\t');
                        std::getline(entry_iss, from, '\t');
                        std::getline(entry_iss, subj, '\t');
                        std::getline(entry_iss, date, '\t');

                        std::cout << std::left << std::setw(6) << id 
                                  << std::setw(20) << from 
                                  << std::setw(30) << subj 
                                  << date << std::endl;
                    }
                    std::cout << "--------------------------------------------------------------------------------\n" << std::endl;
                }
            } else if (subchoice == "2") {
                std::cout << "Enter email ID to read: ";
                std::string id_str;
                std::getline(std::cin, id_str);
                send_command("READ " + id_str);
                std::string read_resp = read_line();
                if (read_resp.rfind("OK", 0) != 0) {
                    std::cout << "[Server Error] " << read_resp << std::endl;
                } else {
                    std::cout << "\n--- Message Content [ID: " << id_str << "] ---" << std::endl;
                    while (true) {
                        std::string mline = read_line();
                        if (mline == ".") break;
                        // Dot-unstuffing: if line starts with '..', strip the leading '.'
                        if (mline.rfind("..", 0) == 0) {
                            mline.erase(0, 1);
                        }
                        std::cout << mline << std::endl;
                    }
                    std::cout << "--- End of Message ---\n" << std::endl;
                }
            } else if (subchoice == "3") {
                std::cout << "Enter email ID to delete: ";
                std::string id_str;
                std::getline(std::cin, id_str);
                send_command("DELETE " + id_str);
                std::string del_resp = read_line();
                std::cout << "[Server] " << del_resp << std::endl;
            } else if (subchoice == "4") {
                send_command("COUNT");
                std::string count_resp = read_line();
                std::cout << "[Server] " << count_resp << std::endl;
            } else if (subchoice == "5") {
                disconnect();
                std::cout << "Logged out successfully.\n" << std::endl;
                break;
            } else {
                std::cout << "Invalid choice. Please enter 1-5." << std::endl;
            }
        }
    }

    void run_menu() {
        while (true) {
            std::cout << "\n========================================" << std::endl;
            std::cout << " SimpleMail Interactive Client" << std::endl;
            std::cout << "========================================" << std::endl;
            std::cout << "1. Send a mail" << std::endl;
            std::cout << "2. Check my mailbox" << std::endl;
            std::cout << "3. Quit" << std::endl;
            std::cout << "> ";

            std::string choice;
            if (!std::getline(std::cin, choice)) break;

            if (choice == "1") {
                handle_send_mail();
            } else if (choice == "2") {
                handle_check_mailbox();
            } else if (choice == "3") {
                disconnect();
                std::cout << "Exiting SimpleMail client. Goodbye!" << std::endl;
                break;
            } else {
                std::cout << "Invalid choice. Please select 1, 2, or 3." << std::endl;
            }
        }
    }
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <port>" << std::endl;
        return 1;
    }

    std::string server_ip = argv[1];
    int port = std::atoi(argv[2]);

    init_networking();

    SimpleMailClient client(server_ip, port);
    if (!client.start()) {
        cleanup_networking();
        return 1;
    }

    client.run_menu();

    cleanup_networking();
    return 0;
}
