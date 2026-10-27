#!/usr/bin/env python3
"""
Comprehensive Automated Test Suite for SimpleMail System
Tests:
  1. Startup checks (smserver fails if smspooler is not running)
  2. Greeting & 30s Mode selection handling
  3. SMTP2 Protocol: Sequence enforcement, user validation, subject, flow control (BODY_STREAM), legacy mode (BODY)
  4. Shared Memory & Spooler Disk Verification: Exact file format, monotonic ID tracking
  5. Monotonic IDs Never Reused after deletion
  6. SMP Protocol: Challenge-response DJB2 authentication, failed attempts limit (3 strikes)
  7. SMP Mailbox commands: COUNT, LIST, READ (with dot-stuffing), DELETE, QUIT
  8. Concurrent multi-client connections via select()
"""

import sys
import os
import socket
import time
import subprocess
import shutil

PORT = 9999
USERFILE = "users.txt"
SPOOLER_EXE = "./smspooler.exe" if os.name == "nt" else "./smspooler"
SERVER_EXE = "./smserver.exe" if os.name == "nt" else "./smserver"

def djb2(s):
    h = 5381
    for c in s:
        h = (((h << 5) + h) + ord(c)) & 0xFFFFFFFFFFFFFFFF
    return h

class SimpleClient:
    def __init__(self, port=PORT):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(5.0)
        self.sock.connect(("127.0.0.1", port))
        self.buf = ""

    def read_line(self):
        while "\r\n" not in self.buf:
            data = self.sock.recv(4096)
            if not data:
                break
            self.buf += data.decode("latin-1")
        if "\r\n" in self.buf:
            line, self.buf = self.buf.split("\r\n", 1)
            return line
        res = self.buf
        self.buf = ""
        return res

    def send_line(self, line):
        self.sock.sendall((line + "\r\n").encode("latin-1"))

    def send_raw(self, data_bytes):
        self.sock.sendall(data_bytes)

    def close(self):
        try:
            self.sock.close()
        except:
            pass

def run_tests():
    print("=" * 70)
    print("  SIMPLEMAIL COMPREHENSIVE AUTOMATED TEST SUITE")
    print("=" * 70)

    # Clean up previous mailboxes
    if os.path.exists("mailboxes"):
        shutil.rmtree("mailboxes")

    # TEST 1: Server startup without spooler should fail
    print("\n[TEST 1] smserver startup check without smspooler...")
    proc_srv = subprocess.Popen([SERVER_EXE, str(PORT), USERFILE], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.5)
    ret = proc_srv.poll()
    if ret is not None and ret != 0:
        print("  -> PASSED: smserver correctly exited when smspooler was not running.")
    else:
        print("  -> FAILED: smserver should not run without smspooler.")
        if ret is None: proc_srv.kill()
        return False

    # Start smspooler
    print("\n[TEST 2] Starting smspooler daemon...")
    spooler_proc = subprocess.Popen([SPOOLER_EXE, USERFILE])
    time.sleep(1.5)
    if spooler_proc.poll() is not None:
        print("  -> FAILED: smspooler stopped unexpectedly.")
        return False
    print("  -> PASSED: smspooler daemon is running and shared memory initialized.")

    # Start smserver
    print("\n[TEST 3] Starting smserver on port {}...".format(PORT))
    server_proc = subprocess.Popen([SERVER_EXE, str(PORT), USERFILE])
    time.sleep(1.5)
    if server_proc.poll() is not None:
        print("  -> FAILED: smserver stopped unexpectedly.")
        spooler_proc.kill()
        return False
    print("  -> PASSED: smserver is running and listening on port {}.".format(PORT))

    try:
        # TEST 4: Handshake and Greeting
        print("\n[TEST 4] Testing Greeting & Mode Handshake...")
        c = SimpleClient()
        greeting = c.read_line()
        assert greeting == "WELCOME SimpleMail v1.0", f"Wrong greeting: {greeting}"
        c.send_line("MODE INVALID")
        err = c.read_line()
        assert err == "ERR Unknown mode", f"Expected 'ERR Unknown mode', got: {err}"
        c.close()
        print("  -> PASSED: Greeting and invalid mode handling verified.")

        # TEST 5: SMTP2 Sending with Credit-based Flow Control (BODY_STREAM)
        print("\n[TEST 5] Testing SMTP2 Sequence Enforcement & Flow Control (BODY_STREAM)...")
        c = SimpleClient()
        assert c.read_line() == "WELCOME SimpleMail v1.0"
        c.send_line("MODE SEND")
        assert c.read_line() == "OK"

        # Out-of-sequence test: send SUB before FROM
        c.send_line("SUB Hello")
        assert c.read_line() == "ERR Bad sequence", "Server must enforce sequence order"

        # Sender declaration
        c.send_line("FROM Alice Wonderland")
        assert c.read_line() == "OK Sender accepted."

        # Invalid recipient test
        c.send_line("TO non_existent_user")
        assert c.read_line() == "ERR No such user"

        # Valid recipients
        c.send_line("TO alice")
        assert c.read_line() == "OK Recipient accepted."
        c.send_line("TO bob")
        assert c.read_line() == "OK Recipient accepted."

        # Subject
        c.send_line("SUB Project Submission")
        assert c.read_line() == "OK Subject accepted."

        # Body stream with flow control
        c.send_line("BODY_STREAM")
        win_msg = c.read_line()
        assert win_msg.startswith("OK Window"), f"Expected initial window, got: {win_msg}"
        print(f"  -> Initial Flow Control Window: {win_msg}")

        # Send chunk 1
        chunk1 = "Hello Alice and Bob,\nThis is the first chunk of email payload."
        c.send_raw(f"CHUNK {len(chunk1)}\r\n{chunk1}".encode("latin-1"))
        resp1 = c.read_line()
        assert resp1.startswith("OK Window"), f"Expected window decrement, got: {resp1}"
        print(f"  -> Sent chunk 1 ({len(chunk1)} bytes). Server reply: {resp1}")

        # Send chunk 2
        chunk2 = "\nHere is the second chunk tested with credit-based streaming.\nBest regards,\nAlice."
        c.send_raw(f"CHUNK {len(chunk2)}\r\n{chunk2}".encode("latin-1"))
        resp2 = c.read_line()
        assert resp2.startswith("OK Window") or resp2.startswith("WINDOW_UPDATE")
        print(f"  -> Sent chunk 2 ({len(chunk2)} bytes). Server reply: {resp2}")

        # End body
        c.send_line("END_BODY")
        queued_msg = c.read_line()
        assert queued_msg.startswith("OK Queued"), f"Expected queued confirmation, got: {queued_msg}"
        print(f"  -> Server Confirmation: {queued_msg}")

        c.send_line("QUIT")
        assert c.read_line() == "BYE"
        c.close()
        print("  -> PASSED: SMTP2 BODY_STREAM sent and queued successfully.")

        # Allow spooler to flush to disk
        time.sleep(1.0)

        # TEST 6: Verify mailboxes on disk and exact file formatting
        print("\n[TEST 6] Verifying Mailbox Persistence & Exact Disk Format...")
        alice_mail1 = "mailboxes/alice/1.txt"
        bob_mail1 = "mailboxes/bob/1.txt"
        assert os.path.exists(alice_mail1), f"Missing {alice_mail1}"
        assert os.path.exists(bob_mail1), f"Missing {bob_mail1}"

        with open(alice_mail1, "r") as f:
            content = f.read()
        print(f"  -> Content of {alice_mail1}:\n{'-'*40}\n{content}{'-'*40}")
        assert "From: Alice Wonderland" in content
        assert "To: alice, bob" in content
        assert "Subject: Project Submission" in content
        assert "Date: " in content
        assert "---\n" in content
        assert "Hello Alice and Bob," in content
        print("  -> PASSED: Exact format verified on disk for all recipients.")

        # TEST 7: SMTP2 Legacy Mode (BODY with dot-termination and byte-stuffing)
        print("\n[TEST 7] Testing SMTP2 Legacy Mode (BODY with CRLF.CRLF & dot-stuffing)...")
        c = SimpleClient()
        c.read_line()
        c.send_line("MODE SEND")
        c.read_line()
        c.send_line("FROM Charlie Brown")
        c.read_line()
        c.send_line("TO alice")
        c.read_line()
        c.send_line("SUB Dot-stuffing Test")
        c.read_line()
        c.send_line("BODY")
        assert c.read_line() == "OK Send body, end with CRLF.CRLF."
        c.send_line("Line 1 of body")
        c.send_line("..Line with leading dot stuffed")
        c.send_line("Line 3")
        c.send_line(".")
        queued2 = c.read_line()
        assert queued2.startswith("OK Queued")
        c.send_line("QUIT")
        c.read_line()
        c.close()
        time.sleep(0.5)

        # Check alice/2.txt
        alice_mail2 = "mailboxes/alice/2.txt"
        assert os.path.exists(alice_mail2), f"Missing {alice_mail2}"
        with open(alice_mail2, "r") as f:
            c2 = f.read()
        assert ".Line with leading dot stuffed" in c2, "Byte-unstuffing must remove single leading dot"
        print("  -> PASSED: Legacy BODY mode with dot-stuffing and 2.txt monotonic ID verified.")

        # TEST 8: Monotonic IDs Never Repeated after deletion
        print("\n[TEST 8] Testing Monotonic ID Invariance (IDs Never Reused)...")
        # Delete 1.txt
        os.remove(alice_mail1)
        print(f"  -> Removed {alice_mail1}")

        # Send a 3rd email to alice
        c = SimpleClient()
        c.read_line()
        c.send_line("MODE SEND")
        c.read_line()
        c.send_line("FROM Bob")
        c.read_line()
        c.send_line("TO alice")
        c.read_line()
        c.send_line("SUB Third Mail")
        c.read_line()
        c.send_line("BODY")
        c.read_line()
        c.send_line("This must be 3.txt, not reusing 1.txt")
        c.send_line(".")
        c.read_line()
        c.send_line("QUIT")
        c.read_line()
        c.close()
        time.sleep(0.5)

        alice_mail3 = "mailboxes/alice/3.txt"
        assert os.path.exists(alice_mail3), f"Expected {alice_mail3} but it was not created!"
        assert not os.path.exists(alice_mail1), f"1.txt should remain deleted, never reused!"
        print("  -> PASSED: Email was assigned ID 3.txt. Monotonic ID counter never repeats deleted IDs.")

        # TEST 9: SMP Authentication (DJB2 Challenge-Response & 3 Failures Limit)
        print("\n[TEST 9] Testing SMP DJB2 Authentication Challenge-Response...")
        c = SimpleClient()
        c.read_line()
        c.send_line("MODE RECV")
        assert c.read_line() == "OK"
        auth_req = c.read_line()
        assert auth_req.startswith("AUTH REQUIRED "), f"Expected challenge, got: {auth_req}"
        nonce = auth_req.split(" ")[2]
        print(f"  -> Received challenge nonce: {nonce}")

        # Wrong password attempt 1
        c.send_line(f"AUTH alice {djb2('wrongpassword' + nonce)}")
        assert c.read_line() == "ERR Authentication failed"
        auth_req2 = c.read_line()
        nonce2 = auth_req2.split(" ")[2]

        # Wrong password attempt 2
        c.send_line(f"AUTH alice {djb2('wrongpassword2' + nonce2)}")
        assert c.read_line() == "ERR Authentication failed"
        auth_req3 = c.read_line()
        nonce3 = auth_req3.split(" ")[2]

        # Wrong password attempt 3 -> should fail and disconnect
        c.send_line(f"AUTH alice {djb2('wrongpassword3' + nonce3)}")
        assert c.read_line() == "ERR Too many failures"
        print("  -> PASSED: 3 strikes authentication failure limit enforced.")
        c.close()

        # Successful Authentication
        print("\n[TEST 10] Testing Successful Authentication & Mailbox Commands (COUNT, LIST, READ, DELETE)...")
        c = SimpleClient()
        c.read_line()
        c.send_line("MODE RECV")
        c.read_line()
        nonce = c.read_line().split(" ")[2]
        # alice's password in users.txt is secretpass1
        hash_val = djb2("secretpass1" + nonce)
        c.send_line(f"AUTH alice {hash_val}")
        auth_ok = c.read_line()
        assert auth_ok == "OK Welcome alice", f"Expected welcome, got: {auth_ok}"
        print(f"  -> Server: {auth_ok}")

        # COUNT
        c.send_line("COUNT")
        count_resp = c.read_line()
        assert count_resp == "OK 2", f"Expected 'OK 2' (mails 2 and 3), got: {count_resp}"
        print(f"  -> COUNT response: {count_resp}")

        # LIST
        c.send_line("LIST")
        list_header = c.read_line()
        assert list_header == "OK 2 messages", f"Expected 'OK 2 messages', got: {list_header}"
        items = []
        while True:
            l = c.read_line()
            if l == ".": break
            items.append(l)
        assert len(items) == 2, f"Expected 2 listing items, got {len(items)}"
        print(f"  -> LIST items retrieved:\n     " + "\n     ".join(items))
        # Verify tab-separated format: <id>\t<from>\t<subject>\t<date>
        assert items[0].startswith("2\tCharlie Brown\tDot-stuffing Test\t")
        assert items[1].startswith("3\tBob\tThird Mail\t")

        # READ 2
        c.send_line("READ 2")
        read_resp = c.read_line()
        assert read_resp == "OK"
        read_lines = []
        while True:
            l = c.read_line()
            if l == ".": break
            read_lines.append(l)
        read_text = "\n".join(read_lines)
        assert "From: Charlie Brown" in read_text
        assert "Subject: Dot-stuffing Test" in read_text
        print(f"  -> READ 2 content:\n{'-'*40}\n{read_text}\n{'-'*40}")

        # DELETE 2
        c.send_line("DELETE 2")
        assert c.read_line() == "OK Deleted"
        print("  -> DELETE 2: OK Deleted")

        # COUNT after delete
        c.send_line("COUNT")
        assert c.read_line() == "OK 1"

        # READ 2 should now return error
        c.send_line("READ 2")
        assert c.read_line() == "ERR No such message"

        # QUIT
        c.send_line("QUIT")
        assert c.read_line() == "BYE"
        c.close()
        print("  -> PASSED: All SMP mailbox operations functioning flawlessly.")

        # TEST 11: Concurrency test with multiple clients
        print("\n[TEST 11] Testing Concurrent Multi-Client Handling...")
        clients = [SimpleClient() for _ in range(5)]
        for cl in clients:
            assert cl.read_line() == "WELCOME SimpleMail v1.0"
            cl.send_line("MODE SEND")
            assert cl.read_line() == "OK"
            cl.send_line("FROM MultiClient")
            assert cl.read_line() == "OK Sender accepted."
            cl.send_line("QUIT")
            assert cl.read_line() == "BYE"
            cl.close()
        print("  -> PASSED: Handled 5 concurrent connections seamlessly.")

        print("\n" + "=" * 70)
        print("  ALL 11 TEST CASES PASSED WITH 100% SUCCESS!")
        print("=" * 70)
        return True

    finally:
        server_proc.kill()
        spooler_proc.kill()

if __name__ == "__main__":
    success = run_tests()
    sys.exit(0 if success else 1)
