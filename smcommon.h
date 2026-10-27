#ifndef SMCOMMON_H
#define SMCOMMON_H

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <cstdint>

#ifdef _WIN32
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <direct.h>
    #include <io.h>
    #define mkdir_compat(path) _mkdir(path)
    #define close_socket(s) closesocket(s)
    typedef SOCKET socket_t;
    #define INVALID_SOCK INVALID_SOCKET
    #define SOCK_ERR SOCKET_ERROR
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <sys/stat.h>
    #include <sys/mman.h>
    #include <fcntl.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <semaphore.h>
    #include <dirent.h>
    #define mkdir_compat(path) mkdir(path, 0755)
    #define close_socket(s) close(s)
    typedef int socket_t;
    #define INVALID_SOCK (-1)
    #define SOCK_ERR (-1)
#endif

// Shared Memory & Queue Constants
#define SHM_NAME "/sm_spool_shm"
#define WIN_SHM_NAME "Local\\sm_spool_shm"
#define WIN_SEM_EMPTY "Local\\sm_sem_empty"
#define WIN_SEM_FULL "Local\\sm_sem_full"
#define WIN_SEM_MUTEX "Local\\sm_sem_mutex"

#define RING_BUFFER_CAPACITY 16
#define MAX_BODY_SIZE 65536
#define MAX_RECIPIENTS 32
#define MAX_USERNAME_LEN 20
#define MAX_PASSWORD_LEN 30
#define MAX_DISPLAY_NAME_LEN 128
#define MAX_SUBJECT_LEN 256
#define MAX_CMD_LEN 512
#define DEFAULT_WINDOW_SIZE 2048

// Message descriptor stored in shared ring buffer
struct MessageSlot {
    char sender_display_name[MAX_DISPLAY_NAME_LEN + 1];
    int recipient_count;
    char recipients[MAX_RECIPIENTS][MAX_USERNAME_LEN + 1];
    char subject[MAX_SUBJECT_LEN + 1];
    char body[MAX_BODY_SIZE + 1];
    size_t body_len;
    uint64_t ticket_id;
    char date_str[32]; // [YYYY-MM-DD HH:MM:SS]
};

// Shared memory structure containing ring buffer and synchronization primitives
struct SharedSpoolRing {
#ifndef _WIN32
    sem_t empty_slots; // Counting semaphore for empty slots
    sem_t full_slots;  // Counting semaphore for full slots
    sem_t ring_mutex;  // Binary semaphore for ring index protection
#else
    // On Windows, named synchronization handles are used
    int dummy_sync;
#endif
    int head; // Enqueue index (producer: smserver)
    int tail; // Dequeue index (consumer: smspooler)
    int capacity;
    MessageSlot slots[RING_BUFFER_CAPACITY];
};

// User record
struct UserRecord {
    std::string username; // Stored in lowercase
    std::string password; // Case-sensitive
};

// Utility: DJB2 Hash function as specified in the PDF (supports both 32-bit and 64-bit unsigned long)
inline unsigned long djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

inline uint64_t djb2_64(const char *str) {
    uint64_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

inline uint32_t djb2_32(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = (unsigned char)*str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

// Utility: Get current timestamp in [YYYY-MM-DD HH:MM:SS] format
inline std::string get_timestamp() {
    std::time_t now = std::time(NULL);
    std::tm *tm_info = std::localtime(&now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "[%Y-%m-%d %H:%M:%S]", tm_info);
    return std::string(buf);
}

// Utility: Format timestamp without brackets: YYYY-MM-DD HH:MM:SS
inline std::string get_datetime_str() {
    std::time_t now = std::time(NULL);
    std::tm *tm_info = std::localtime(&now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buf);
}

// Utility: Log message to stdout with timestamp
inline void log_msg(const std::string &msg) {
    std::cout << get_timestamp() << " " << msg << std::endl;
}

// Utility: Convert string to lowercase
inline std::string to_lower(const std::string &str) {
    std::string res = str;
    for (char &c : res) {
        c = (char)std::tolower((unsigned char)c);
    }
    return res;
}

// Utility: Validate username (1-20 lowercase letters)
inline bool is_valid_username(const std::string &u) {
    if (u.empty() || u.length() > MAX_USERNAME_LEN) return false;
    for (char c : u) {
        if (c < 'a' || c > 'z') return false;
    }
    return true;
}

// Utility: Validate password (1-30 alphanumeric characters)
inline bool is_valid_password(const std::string &p) {
    if (p.empty() || p.length() > MAX_PASSWORD_LEN) return false;
    for (char c : p) {
        if (!std::isalnum((unsigned char)c)) return false;
    }
    return true;
}

// Utility: Generate random 8-character alphanumeric nonce
inline std::string generate_nonce() {
    static const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string nonce;
    nonce.reserve(8);
    for (int i = 0; i < 8; ++i) {
        nonce.push_back(charset[std::rand() % (sizeof(charset) - 1)]);
    }
    return nonce;
}

// Sockets initialization wrapper (for Windows compatibility)
inline bool init_networking() {
#ifdef _WIN32
    WSADATA wsa;
    return (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
    return true;
#endif
}

inline void cleanup_networking() {
#ifdef _WIN32
    WSACleanup();
#endif
}

// Shared memory synchronization abstraction
struct SpoolIPC {
    SharedSpoolRing *ring;
#ifdef _WIN32
    HANDLE hMap;
    HANDLE hSemEmpty;
    HANDLE hSemFull;
    HANDLE hSemMutex;
#else
    int shm_fd;
#endif

    SpoolIPC() : ring(nullptr) {
#ifdef _WIN32
        hMap = NULL;
        hSemEmpty = NULL;
        hSemFull = NULL;
        hSemMutex = NULL;
#else
        shm_fd = -1;
#endif
    }
};

// Spooler creates/initializes shared memory and semaphores
inline bool init_spool_shm_creator(SpoolIPC &ipc) {
#ifdef _WIN32
    ipc.hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SharedSpoolRing), WIN_SHM_NAME);
    if (!ipc.hMap) return false;
    ipc.ring = (SharedSpoolRing *)MapViewOfFile(ipc.hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedSpoolRing));
    if (!ipc.ring) return false;

    memset(ipc.ring, 0, sizeof(SharedSpoolRing));
    ipc.ring->head = 0;
    ipc.ring->tail = 0;
    ipc.ring->capacity = RING_BUFFER_CAPACITY;

    ipc.hSemEmpty = CreateSemaphoreA(NULL, RING_BUFFER_CAPACITY, RING_BUFFER_CAPACITY, WIN_SEM_EMPTY);
    ipc.hSemFull = CreateSemaphoreA(NULL, 0, RING_BUFFER_CAPACITY, WIN_SEM_FULL);
    ipc.hSemMutex = CreateSemaphoreA(NULL, 1, 1, WIN_SEM_MUTEX);
    return (ipc.hSemEmpty && ipc.hSemFull && ipc.hSemMutex);
#else
    // POSIX Implementation
    shm_unlink(SHM_NAME); // Clean up if existing
    ipc.shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (ipc.shm_fd < 0) return false;
    if (ftruncate(ipc.shm_fd, sizeof(SharedSpoolRing)) < 0) return false;

    ipc.ring = (SharedSpoolRing *)mmap(NULL, sizeof(SharedSpoolRing), PROT_READ | PROT_WRITE, MAP_SHARED, ipc.shm_fd, 0);
    if (ipc.ring == MAP_FAILED) {
        ipc.ring = nullptr;
        return false;
    }

    memset(ipc.ring, 0, sizeof(SharedSpoolRing));
    ipc.ring->head = 0;
    ipc.ring->tail = 0;
    ipc.ring->capacity = RING_BUFFER_CAPACITY;

    // Initialize POSIX unnamed process-shared semaphores
    if (sem_init(&ipc.ring->empty_slots, 1, RING_BUFFER_CAPACITY) < 0) return false;
    if (sem_init(&ipc.ring->full_slots, 1, 0) < 0) return false;
    if (sem_init(&ipc.ring->ring_mutex, 1, 1) < 0) return false;

    return true;
#endif
}

// Server connects to existing shared memory and semaphores
inline bool init_spool_shm_client(SpoolIPC &ipc) {
#ifdef _WIN32
    ipc.hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, WIN_SHM_NAME);
    if (!ipc.hMap) return false;
    ipc.ring = (SharedSpoolRing *)MapViewOfFile(ipc.hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedSpoolRing));
    if (!ipc.ring) return false;

    ipc.hSemEmpty = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, WIN_SEM_EMPTY);
    ipc.hSemFull = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, WIN_SEM_FULL);
    ipc.hSemMutex = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, WIN_SEM_MUTEX);
    return (ipc.hSemEmpty && ipc.hSemFull && ipc.hSemMutex);
#else
    // POSIX Implementation
    ipc.shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (ipc.shm_fd < 0) return false;

    ipc.ring = (SharedSpoolRing *)mmap(NULL, sizeof(SharedSpoolRing), PROT_READ | PROT_WRITE, MAP_SHARED, ipc.shm_fd, 0);
    if (ipc.ring == MAP_FAILED) {
        ipc.ring = nullptr;
        return false;
    }
    return true;
#endif
}

// Synchronization helpers
inline void spool_wait_empty(SpoolIPC &ipc) {
#ifdef _WIN32
    WaitForSingleObject(ipc.hSemEmpty, INFINITE);
#else
    sem_wait(&ipc.ring->empty_slots);
#endif
}

inline void spool_post_empty(SpoolIPC &ipc) {
#ifdef _WIN32
    ReleaseSemaphore(ipc.hSemEmpty, 1, NULL);
#else
    sem_post(&ipc.ring->empty_slots);
#endif
}

inline void spool_wait_full(SpoolIPC &ipc) {
#ifdef _WIN32
    WaitForSingleObject(ipc.hSemFull, INFINITE);
#else
    sem_wait(&ipc.ring->full_slots);
#endif
}

inline void spool_post_full(SpoolIPC &ipc) {
#ifdef _WIN32
    ReleaseSemaphore(ipc.hSemFull, 1, NULL);
#else
    sem_post(&ipc.ring->full_slots);
#endif
}

inline void spool_lock_mutex(SpoolIPC &ipc) {
#ifdef _WIN32
    WaitForSingleObject(ipc.hSemMutex, INFINITE);
#else
    sem_wait(&ipc.ring->ring_mutex);
#endif
}

inline void spool_unlock_mutex(SpoolIPC &ipc) {
#ifdef _WIN32
    ReleaseSemaphore(ipc.hSemMutex, 1, NULL);
#else
    sem_post(&ipc.ring->ring_mutex);
#endif
}

inline void cleanup_spool_ipc(SpoolIPC &ipc, bool is_creator) {
#ifdef _WIN32
    (void)is_creator;
    if (ipc.ring) UnmapViewOfFile(ipc.ring);
    if (ipc.hMap) CloseHandle(ipc.hMap);
    if (ipc.hSemEmpty) CloseHandle(ipc.hSemEmpty);
    if (ipc.hSemFull) CloseHandle(ipc.hSemFull);
    if (ipc.hSemMutex) CloseHandle(ipc.hSemMutex);
#else
    if (ipc.ring) {
        if (is_creator) {
            sem_destroy(&ipc.ring->empty_slots);
            sem_destroy(&ipc.ring->full_slots);
            sem_destroy(&ipc.ring->ring_mutex);
        }
        munmap(ipc.ring, sizeof(SharedSpoolRing));
    }
    if (ipc.shm_fd >= 0) close(ipc.shm_fd);
    if (is_creator) {
        shm_unlink(SHM_NAME);
    }
#endif
}

#endif // SMCOMMON_H
