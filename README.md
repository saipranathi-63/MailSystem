# SimpleMail – A Concurrent, Distributed Mail System with Shared-Memory Spooling & Flow Control

SimpleMail is a complete, robust, multi-process command-line email system implemented in C++ (compatible with standard C++11/C++14/C++17 and Dev-C++ / GCC / Clang). The system implements POSIX / System V shared memory, process-shared semaphores, non-blocking I/O multiplexing with `select()`, application-layer credit-based flow control (SMTP2), and challenge-response authentication using DJB2 hashing (SMP).

---

## 1. System Architecture Overview

The system consists of three interacting programs:

1. **`smspooler` (SimpleMail Background Spooler Daemon)**:
   - Initializes the shared memory segment (`SM_SPOOL`) and synchronization semaphores.
   - Manages the `mailboxes/` directory hierarchy on disk (`mailboxes/<username>/`).
   - Maintains monotonic integer message IDs per user starting from 1 (e.g., `1.txt`, `2.txt`) ensuring IDs **never repeat**, even after file deletion.
   - Continuously dequeues messages from the shared memory circular ring buffer, formats them into standard email files, and writes them to disk.

2. **`smserver` (SimpleMail Frontend Network Server)**:
   - Strictly decoupled from direct disk writing (forbidden from writing directly to `mailboxes/`).
   - Connects to the `SM_SPOOL` shared memory ring buffer as a producer.
   - Listens on a configurable TCP port, handling multiple concurrent clients using `select()`.
   - Implements a 30-second mode selection timeout.
   - Handles the **SMTP2** sending protocol with application-layer credit-based flow control windowing (`BODY_STREAM`) and legacy dot-terminated mode (`BODY`).
   - Handles the **SMP** retrieval protocol with DJB2 challenge-response authentication (3-attempt maximum) and mailbox operations (`COUNT`, `LIST`, `READ`, `DELETE`, `QUIT`).
   - Enqueues fully streamed messages into `SM_SPOOL` and assigns unique monotonic ticket IDs.

3. **`smclient` (SimpleMail Interactive Client)**:
   - Connects to `smserver` via TCP.
   - Interactive terminal menu providing:
     - **Option 1 (Send a mail)**: Prompts for sender display name, recipient usernames, subject, and body text. Handles credit windowing (`BODY_STREAM`) or legacy mode (`BODY`) transparently with real-time status updates.
     - **Option 2 (Check mailbox)**: Prompts for username and password, performs the challenge-response DJB2 hash handshake, and opens the mailbox sub-menu (`LIST`, `READ`, `DELETE`, `COUNT`, `Logout`).
     - **Option 3 (Quit)**.

---

## 2. Shared Memory Spool Ring (`SM_SPOOL`) Layout

Inter-process communication between `smserver` (producer) and `smspooler` (consumer) is decoupled via a circular ring buffer of fixed capacity $K = 16$ slots maintained in shared memory.

### Data Structures (`smcommon.h`)

```cpp
#define RING_BUFFER_CAPACITY 16
#define MAX_BODY_SIZE 65536
#define MAX_RECIPIENTS 32
#define MAX_USERNAME_LEN 20
#define MAX_DISPLAY_NAME_LEN 128
#define MAX_SUBJECT_LEN 256

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

struct SharedSpoolRing {
#ifndef _WIN32
    sem_t empty_slots; // Counting semaphore for empty slots (init: K)
    sem_t full_slots;  // Counting semaphore for full slots (init: 0)
    sem_t ring_mutex;  // Binary semaphore for index pointer protection (init: 1)
#else
    int dummy_sync;
#endif
    int head; // Producer (smserver) insert index
    int tail; // Consumer (smspooler) extract index
    int capacity;
    MessageSlot slots[RING_BUFFER_CAPACITY];
};
```

---

## 3. Semaphore Synchronization Design

Coordination between producer (`smserver`) and consumer (`smspooler`) protects against race conditions using a classic bounded-buffer producer-consumer pattern:

1. **`empty_slots` Counting Semaphore**: Initialized to $K = 16$. Tracks available empty slots for `smserver` to write into.
2. **`full_slots` Counting Semaphore**: Initialized to $0$. Tracks filled slots ready for `smspooler` to read from.
3. **`ring_mutex` Binary Semaphore / Mutex**: Initialized to $1$. Protects updates to `head` and `tail` pointers.

### Producer Workflow (`smserver`):
```text
wait(empty_slots)   // Decrements empty slot counter; blocks if buffer is full
wait(ring_mutex)    // Locks critical section
slots[head] = message
head = (head + 1) % K
post(ring_mutex)    // Unlocks critical section
post(full_slots)    // Increments full slot counter; wakes smspooler
```

### Consumer Workflow (`smspooler`):
```text
wait(full_slots)    // Decrements full slot counter; blocks if buffer is empty
wait(ring_mutex)    // Locks critical section
message = slots[tail]
tail = (tail + 1) % K
post(ring_mutex)    // Unlocks critical section
post(empty_slots)   // Increments empty slot counter; signals smserver
// Disk persistence: writes to mailboxes/<recipient>/<id>.txt
```

---

## 4. Flow Control State Machine (SMTP2 Credit-Based Windowing)

Rather than uncontrolled packet flooding, SMTP2 implements application-level credit-based window flow control (mirroring transport-layer sliding window protocols):

```
       Client                                             Server
         |                                                  |
         |----------------- BODY_STREAM ------------------->|
         |<------------ OK Window 2048 ---------------------| (Initial Window)
         |                                                  |
         |--- CHUNK 512 + [512 bytes payload] ------------->|
         |<------------ OK Window 1536 ---------------------| (Window Decremented)
         |                                                  |
         |--- CHUNK 1024 + [1024 bytes payload] ----------->|
         |<------------ OK Window 512 ----------------------|
         |<-------- WINDOW_UPDATE 2048 (Unsolicited) -------| (Credit Replenishment)
         |                                                  |
         |--- CHUNK 512 + [512 bytes payload] ------------->|
         |<------------ OK Window 2048 ---------------------|
         |                                                  |
         |------------------- END_BODY -------------------->|
         |<----- OK Queued <ticket_id> to N mailboxes ------|
```

### Zero-Window Stall & Backpressure:
- If available credits reach 0 (`Window 0`), the client **must halt transmission**.
- If a client transmits when credit is 0, the server drops the connection with:
  `ERR Window overflow\r\n`
- As the server enqueues data and the spooler drains buffers, the server asynchronously transmits an unsolicited credit update frame:
  `WINDOW_UPDATE <additional_credits>\r\n`
- The client receives this frame, adds `<additional_credits>` to its balance, and resumes sending chunks.

---

## 5. Protocols Specification Summary

### Common Handshake & Mode Selection
- On connection, server greets: `WELCOME SimpleMail v1.0\r\n`.
- Client declares mode within 30 seconds:
  - `MODE SEND\r\n` -> Server replies `OK\r\n` (enters SMTP2).
  - `MODE RECV\r\n` -> Server replies `OK\r\n` followed by challenge `AUTH REQUIRED <nonce>\r\n` (enters SMP).
  - Timeout: after 30 seconds without mode declaration, server closes the connection.

### SMTP2 (Sending Mail)
1. `FROM <display_name>\r\n` -> `OK Sender accepted.\r\n`
2. `TO <username>\r\n` -> `OK Recipient accepted.\r\n` or `ERR No such user\r\n` (at least 1 valid recipient required).
3. `SUB <subject_text>\r\n` -> `OK Subject accepted.\r\n` (empty stored as `(no subject)`).
4. Body:
   - `BODY_STREAM\r\n` -> `OK Window <bytes>\r\n` -> chunks -> `END_BODY\r\n`.
   - OR `BODY\r\n` -> `OK Send body, end with CRLF.CRLF.\r\n` -> lines with dot-stuffing -> `.\r\n`.
5. Enqueue & confirmation: `OK Queued <ticket_id> to N mailboxes\r\n`.
6. `QUIT\r\n` -> `BYE\r\n`.

### SMP (Mail Retrieval)
1. Challenge: `AUTH REQUIRED <nonce>\r\n` (8-character alphanumeric).
2. Response: `AUTH <username> <hash>\r\n`, where `hash = djb2(password + nonce)`:
   ```cpp
   unsigned long djb2(const char *str) {
       unsigned long hash = 5381;
       int c;
       while ((c = *str++))
           hash = ((hash << 5) + hash) + c;
       return hash;
   }
   ```
3. Mailbox Commands:
   - `COUNT\r\n` -> `OK <count>\r\n`.
   - `LIST\r\n` -> `OK <count> messages\r\n`, then lines of `<id>\t<from>\t<subject>\t<date>\r\n`, ended with `.\r\n`.
   - `READ <id>\r\n` -> `OK\r\n`, raw mail contents with dot-stuffing, ended with `.\r\n`.
   - `DELETE <id>\r\n` -> `OK Deleted\r\n`. (IDs are never reused!).
   - `QUIT\r\n` -> `BYE\r\n`.

---

## 6. Disk Mail File Format

Committed by `smspooler` to `mailboxes/<recipient>/<id>.txt`:
```
From: <display_name>
To: <comma-separated list of accepted recipients>
Subject: <subject_text>
Date: <YYYY-MM-DD HH:MM:SS>
---
<body text>
```

---

## 7. How to Compile and Run

### A. Using Dev-C++ on Windows
1. Launch Dev-C++.
2. You can open `smserver.cpp`, `smspooler.cpp`, and `smclient.cpp` as individual files or create a Console Application project.
3. In Dev-C++, go to **Project Options** -> **Parameters** -> **Linker** (or **Tools** -> **Compiler Options**):
   - Add: `-lws2_32`
   - Make sure C++11 or higher is enabled (`-std=c++11`).
4. Press **F9** or **F11** to Compile & Run.

### B. Using Terminal (Windows PowerShell / Command Prompt)
Navigate to the directory:
```powershell
cd "c:\Users\SAI PRANATHI\Documents\Semesters\CN\MailSystem"
```

Compile with Dev-C++'s bundled MinGW g++:
```powershell
& "C:\Program Files (x86)\Dev-Cpp\MinGW64\bin\g++.exe" -std=c++11 -Wall -O2 smspooler.cpp -o smspooler.exe -lws2_32
& "C:\Program Files (x86)\Dev-Cpp\MinGW64\bin\g++.exe" -std=c++11 -Wall -O2 smserver.cpp -o smserver.exe -lws2_32
& "C:\Program Files (x86)\Dev-Cpp\MinGW64\bin\g++.exe" -std=c++11 -Wall -O2 smclient.cpp -o smclient.exe -lws2_32
```

Launch components in separate terminal tabs in the exact required order:
1. **Terminal 1 (Spooler Daemon)**:
   ```powershell
   .\smspooler.exe users.txt
   ```
2. **Terminal 2 (Server)**:
   ```powershell
   .\smserver.exe 8080 users.txt
   ```
3. **Terminal 3 (Interactive Client)**:
   ```powershell
   .\smclient.exe 127.0.0.1 8080
   ```

### C. Using Linux / WSL / macOS
Simply run the included `Makefile`:
```bash
make
```
Then start the processes in order:
```bash
# Terminal 1:
./smspooler users.txt

# Terminal 2:
./smserver 8080 users.txt

# Terminal 3:
./smclient 127.0.0.1 8080
```

---

## 8. Automated Test Suite Execution & Output

An automated end-to-end Python test script `test_suite.py` is included to test all features against the running binaries.

### Running the Test Suite:
```powershell
python test_suite.py
```

### Actual Test Suite Execution Log:
```text
======================================================================
  SIMPLEMAIL COMPREHENSIVE AUTOMATED TEST SUITE
======================================================================

[TEST 1] smserver startup check without smspooler...
  -> PASSED: smserver correctly exited when smspooler was not running.

[TEST 2] Starting smspooler daemon...
  -> PASSED: smspooler daemon is running and shared memory initialized.

[TEST 3] Starting smserver on port 9999...
  -> PASSED: smserver is running and listening on port 9999.

[TEST 4] Testing Greeting & Mode Handshake...
  -> PASSED: Greeting and invalid mode handling verified.

[TEST 5] Testing SMTP2 Sequence Enforcement & Flow Control (BODY_STREAM)...
  -> Initial Flow Control Window: OK Window 2048
  -> Sent chunk 1 (62 bytes). Server reply: OK Window 1986
  -> Sent chunk 2 (82 bytes). Server reply: OK Window 1904
  -> Server Confirmation: OK Queued 1 to 2 mailboxes
  -> PASSED: SMTP2 BODY_STREAM sent and queued successfully.

[TEST 6] Verifying Mailbox Persistence & Exact Disk Format...
  -> Content of mailboxes/alice/1.txt:
----------------------------------------
From: Alice Wonderland
To: alice, bob
Subject: Project Submission
Date: 2026-10-28 01:05:01
---
Hello Alice and Bob,
This is the first chunk of email payload.
Here is the second chunk tested with credit-based streaming.
Best regards,
Alice.
----------------------------------------
  -> PASSED: Exact format verified on disk for all recipients.

[TEST 7] Testing SMTP2 Legacy Mode (BODY with CRLF.CRLF & dot-stuffing)...
  -> PASSED: Legacy BODY mode with dot-stuffing and 2.txt monotonic ID verified.

[TEST 8] Testing Monotonic ID Invariance (IDs Never Reused)...
  -> Removed mailboxes/alice/1.txt
  -> PASSED: Email was assigned ID 3.txt. Monotonic ID counter never repeats deleted IDs.

[TEST 9] Testing SMP DJB2 Authentication Challenge-Response...
  -> Received challenge nonce: FRaqbC8w
  -> PASSED: 3 strikes authentication failure limit enforced.

[TEST 10] Testing Successful Authentication & Mailbox Commands (COUNT, LIST, READ, DELETE)...
  -> Server: OK Welcome alice
  -> COUNT response: OK 2
  -> LIST items retrieved:
     2	Charlie Brown	Dot-stuffing Test	2026-10-28 01:05:02
     3	Bob	Third Mail	2026-10-28 01:05:03
  -> READ 2 content:
----------------------------------------
From: Charlie Brown
To: alice
Subject: Dot-stuffing Test
Date: 2026-10-28 01:05:02
---
Line 1 of body
..Line with leading dot stuffed
Line 3
----------------------------------------
  -> DELETE 2: OK Deleted
  -> PASSED: All SMP mailbox operations functioning flawlessly.

[TEST 11] Testing Concurrent Multi-Client Handling...
  -> PASSED: Handled 5 concurrent connections seamlessly.

======================================================================
  ALL 11 TEST CASES PASSED WITH 100% SUCCESS!
======================================================================
```

---

## 9. Deliverables File Manifest

- [`smserver.cpp`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/smserver.cpp) / [`smserver.c`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/smserver.c): Frontend TCP network server with `select()` concurrency and flow control.
- [`smspooler.cpp`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/smspooler.cpp) / [`smspooler.c`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/smspooler.c): Background spooler daemon managing disk mailboxes, monotonic ID counters, and shared memory consumer.
- [`smclient.cpp`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/smclient.cpp) / [`smclient.c`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/smclient.c): Interactive terminal client with menu system and streaming support.
- [`smcommon.h`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/smcommon.h): Shared memory layout, DJB2 hash, IPC abstractions, constants, and utilities.
- [`Makefile`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/Makefile): Cross-platform build script.
- [`users.txt`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/users.txt): User registry file.
- [`test_suite.py`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/test_suite.py): Comprehensive 11-step automated verification suite.
- [`README.md`](file:///c:/Users/SAI%20PRANATHI/Documents/Semesters/CN/MailSystem/README.md): Architecture and protocol documentation.
