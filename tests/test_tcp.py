"""Localhost integration checks using ephemeral ports and the real C server."""
import contextlib
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

CLIENT = sys.argv[1] if len(sys.argv) > 1 else "./build/client"
SERVER = sys.argv[2] if len(sys.argv) > 2 else "./build/server"
HEADER = struct.Struct("!IIIIQQ")
MAGIC = 0x4D4D4733


def pack(kind, first, second, status=0):
    return HEADER.pack(MAGIC, 1, kind, status, first, second)


def receive(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("Incomplete response")
        data.extend(chunk)
    return bytes(data)


def header(sock):
    result = HEADER.unpack(receive(sock, HEADER.size))
    if result[:2] != (MAGIC, 1):
        raise ValueError("Invalid protocol magic/version")
    return result[2:]


class TcpTests(unittest.TestCase):
    @contextlib.contextmanager
    def server(self, block=4096, working_set=16384):
        process = subprocess.Popen(
            [SERVER, "--port", "0", "--block-size", str(block),
             "--working-set", str(working_set)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            ready, _, _ = select.select([process.stdout], [], [], 10)
            if not ready:
                self.fail("Server did not report readiness within 10 seconds")
            line = process.stdout.readline().strip()
            self.assertTrue(line.startswith("READY "), line)
            fields = dict(item.split("=", 1) for item in line.split()[1:])
            yield int(fields["port"])
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                _, errors = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                _, errors = process.communicate()
            self.assertNotIn("AddressSanitizer", errors, errors)
            self.assertNotIn("runtime error:", errors, errors)

    def client(self, port, block=4096, capacity=8192, working_set=16384,
               accesses=4096, backend="tcp"):
        return subprocess.run(
            [CLIENT, "--backend", backend, "--host", "127.0.0.1", "--port", str(port),
             "--block-size", str(block), "--cache-size", str(capacity),
             "--working-set", str(working_set), "--accesses", str(accesses)],
            capture_output=True, text=True, timeout=20)

    def output(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def test_same_results_as_synthetic_for_all_block_sizes(self):
        for block in (4096, 16384, 65536, 262144):
            with self.subTest(block=block), self.server(block, 1048576) as port:
                settings = dict(block=block, capacity=524288, working_set=1048576,
                                accesses=262144)
                local = self.output(self.client(port, backend="synthetic", **settings))
                remote = self.output(self.client(port, **settings))
                for key in ("requests", "hits", "misses", "evictions", "load_errors", "checksum"):
                    self.assertEqual(remote[key], local[key], key)
                self.assertEqual(remote["backend"], "tcp")
                self.assertEqual(int(remote["tcp_connections"]), 1)
                self.assertEqual(int(remote["tcp_requests"]), int(remote["misses"]))
                self.assertEqual(int(remote["tcp_payload_bytes"]), int(remote["misses"]) * block)

    def test_mismatched_configuration_is_rejected(self):
        with self.server() as port:
            result = self.client(port, block=8192)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("handshake", result.stderr)
            self.output(self.client(port))  # Server survives the rejected client.

    def test_fragmented_requests_and_exact_payload(self):
        with self.server() as port:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                message = pack(1, 4096, 16384)
                for piece in (message[:3], message[3:13], message[13:]):
                    sock.sendall(piece)
                    time.sleep(0.002)
                self.assertEqual(header(sock), (3, 0, 4096, 16384))
                message = pack(2, 2, 0)
                for piece in (message[:1], message[1:18], message[18:]):
                    sock.sendall(piece)
                    time.sleep(0.002)
                self.assertEqual(header(sock), (4, 0, 2, 4096))
                self.assertEqual(receive(sock, 4096), bytes(i % 251 for i in range(8192, 12288)))

    def test_invalid_block_id(self):
        with self.server() as port:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                sock.sendall(pack(1, 4096, 16384))
                self.assertEqual(header(sock), (3, 0, 4096, 16384))
                sock.sendall(pack(2, 4, 0))
                self.assertEqual(header(sock), (4, 3, 4, 0))
            self.output(self.client(port))

    def test_server_survives_truncated_header(self):
        with self.server() as port:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
                sock.sendall(pack(1, 4096, 16384)[:9])
                sock.shutdown(socket.SHUT_WR)
                self.assertEqual(sock.recv(1), b"")
            self.output(self.client(port))

    def test_client_rejects_truncated_block(self):
        failures = []
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)
            port = listener.getsockname()[1]

            def broken_server():
                try:
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(5)
                        self.assertEqual(header(conn), (1, 0, 4096, 16384))
                        reply = pack(3, 4096, 16384)
                        for piece in (reply[:5], reply[5:19], reply[19:]):
                            conn.sendall(piece)
                            time.sleep(0.002)
                        self.assertEqual(header(conn), (2, 0, 0, 0))
                        conn.sendall(pack(4, 0, 4096) + b"incomplete")
                except BaseException as error:
                    failures.append(error)

            worker = threading.Thread(target=broken_server, daemon=True)
            worker.start()
            result = self.client(port)
            worker.join(timeout=6)
            self.assertFalse(worker.is_alive())
            self.assertEqual(failures, [])
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("TCP block transfer failed", result.stderr)
            self.assertNotIn("requests=", result.stdout)

    def test_missing_server_is_an_error(self):
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))  # Bound, deliberately not listening.
            result = self.client(unused.getsockname()[1])
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("TCP connection", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
