"""B0 integration: real warmup, no measured misses/I/O, unchanged trace."""
from pathlib import Path
import csv
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

import test_tcp

CLIENT = sys.argv[1] if len(sys.argv) > 1 else "./build/client"
SERVER = sys.argv[2] if len(sys.argv) > 2 else "./build/server"
SD_MODE = len(sys.argv) > 3 and sys.argv[3] == "sd"
GENERATOR = Path(__file__).resolve().parents[1] / "scripts" / "prepare_dataset.py"


class Common(unittest.TestCase):
    def setUp(self):
        Path("build").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="b0-", dir="build")
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name)

    def client(self, baseline="b0", pattern="sequential", block=4096, working_set=16384,
               capacity=None, accesses=4096, backend="synthetic", port=18083,
               seed=42, extra=(), data=None):
        if capacity is None:
            capacity = working_set
        command = [CLIENT, "--baseline", baseline, "--backend", backend,
                   "--pattern", pattern, "--seed", str(seed), "--block-size", str(block),
                   "--cache-size", str(capacity), "--working-set", str(working_set),
                   "--accesses", str(accesses), "--port", str(port)]
        if data is not None:
            command += ["--file", str(data)]
        return subprocess.run([*command, *extra], capture_output=True, text=True, timeout=40)

    def output(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("runtime error:", result.stderr)
        self.assertNotIn("AddressSanitizer", result.stderr)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def verify_b0(self, output, accesses=4096, blocks=4, metrics=True):
        for key, expected in {"baseline": "b0", "b0_verified": "1", "cache_start": "warm_full",
                              "requests": str(accesses), "hits": str(accesses), "misses": "0",
                              "evictions": "0", "load_errors": "0", "hit_rate": "1.000000000",
                              "miss_rate": "0.000000000", "warmup_requests": str(blocks),
                              "warmup_misses": str(blocks), "warmup_hits": "0",
                              "warmup_evictions": "0", "warmup_load_errors": "0",
                              "tcp_requests": "0", "tcp_payload_bytes": "0", "sd_requests": "0",
                              "sd_read_calls": "0", "sd_read_bytes": "0"}.items():
            self.assertEqual(output[key], expected, key)
        if metrics:
            self.assertEqual(output["latency_all_count"], str(accesses))
            self.assertEqual(output["latency_hit_count"], str(accesses))
            self.assertEqual(output["latency_all_sum_ns"], output["latency_hit_sum_ns"])
            self.assertEqual(output["latency_all_mean_ns"], output["latency_hit_mean_ns"])
            for group in ("miss", "backend"):
                self.assertEqual(output[f"latency_{group}_count"], "0")
                for metric in ("mean_ns", "p50_ns", "p95_ns", "p99_ns"):
                    self.assertEqual(output[f"latency_{group}_{metric}"], "na")
            self.assertEqual(output["usage_resident_blocks"], str(blocks))
            self.assertEqual(output["usage_evicted_blocks"], "0")
            self.assertEqual(output["block_utilization_evicted"], "na")


class B0Tests(Common):
    def test_zero_misses_for_patterns_and_seeds(self):
        for pattern in ("sequential", "uniform", "zipf"):
            for seed in (0, 42, 18446744073709551615):
                with self.subTest(pattern=pattern, seed=seed):
                    self.verify_b0(self.output(self.client(pattern=pattern, seed=seed)))

    def test_warmup_does_not_consume_trace_or_checksum(self):
        for pattern in ("sequential", "uniform", "zipf"):
            with self.subTest(pattern=pattern):
                cold = self.output(self.client(baseline="none", pattern=pattern))
                warm = self.output(self.client(pattern=pattern))
                for key in ("trace_hash", "checksum", "seed", "hot20_access_fraction", "requests"):
                    self.assertEqual(cold[key], warm[key], key)
                self.assertGreater(int(cold["misses"]), 0)
                self.assertEqual(cold["warmup_requests"], "0")
                self.assertEqual(cold["cache_start"], "empty")

    def test_coverage_excludes_warmup_and_csv_contains_only_measured_hits(self):
        path = self.folder / "latencies.csv"
        output = self.output(self.client(accesses=3, extra=["--latency-csv", str(path)]))
        self.verify_b0(output, accesses=3)
        self.assertEqual(output["usage_resident_unique_bytes"], "24")
        self.assertAlmostEqual(float(output["block_utilization_resident"]), 24 / 16384, places=9)
        with path.open(newline="") as file:
            rows = list(csv.DictReader(file))
        self.assertEqual(len(rows), 3)
        self.assertEqual([row["access_index"] for row in rows], ["0", "1", "2"])
        self.assertTrue(all(row["was_hit"] == "1" and row["backend_ns"] == "0" for row in rows))

    def test_block_sizes_preserve_trace_and_zero_misses(self):
        traces, checksums = set(), set()
        for block in (4096, 16384, 65536, 262144):
            output = self.output(self.client(pattern="uniform", block=block, working_set=1048576))
            self.verify_b0(output, blocks=1048576 // block)
            traces.add(output["trace_hash"])
            checksums.add(output["checksum"])
        self.assertEqual(len(traces), 1)
        self.assertEqual(len(checksums), 1)

    def test_metrics_off_still_verifies_b0(self):
        output = self.output(self.client(pattern="zipf", extra=["--metrics", "off"]))
        self.verify_b0(output, metrics=False)
        self.assertEqual(output["metrics_enabled"], "0")
        self.assertNotIn("latency_all_count", output)

    def test_wrong_capacity_and_invalid_baseline_are_rejected(self):
        for capacity in (8192, 32768):
            result = self.client(capacity=capacity)
            self.assertEqual(result.returncode, 1)
            self.assertIn("B0 requires cache-size equal", result.stderr)
        for extra in (["--baseline", "unknown"], ["--baseline"]):
            result = self.client(extra=extra)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Invalid baseline", result.stderr)

    def test_tcp_only_loads_blocks_during_warmup(self):
        test_tcp.SERVER = SERVER
        harness = test_tcp.TcpTests()
        for pattern in ("sequential", "uniform", "zipf"):
            with self.subTest(pattern=pattern), harness.server() as port:
                output = self.output(self.client(backend="tcp", pattern=pattern, port=port))
                self.verify_b0(output)
                self.assertEqual(output["warmup_tcp_requests"], "4")
                self.assertEqual(output["warmup_tcp_payload_bytes"], "16384")
                self.assertEqual(output["tcp_connections"], "1")

    def test_failed_warmup_never_exports_measurement(self):
        failures = []
        csv_path = self.folder / "must-not-exist.csv"
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)

            def broken_server():
                try:
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(5)
                        self.assertEqual(test_tcp.header(conn), (1, 0, 4096, 16384))
                        conn.sendall(test_tcp.pack(3, 4096, 16384))
                        self.assertEqual(test_tcp.header(conn), (2, 0, 0, 0))
                        conn.sendall(test_tcp.pack(4, 0, 4096) + b"truncated")
                except BaseException as error:
                    failures.append(error)

            worker = threading.Thread(target=broken_server, daemon=True)
            worker.start()
            result = self.client(backend="tcp", port=listener.getsockname()[1],
                                 extra=["--latency-csv", str(csv_path)])
            worker.join(timeout=6)
            self.assertFalse(worker.is_alive())
            self.assertEqual(failures, [])
        self.assertEqual(result.returncode, 1)
        self.assertIn("B0 warmup failed", result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertFalse(csv_path.exists())


class SdB0Tests(Common):
    def test_sd_warms_once_then_zero_measured_io_for_all_patterns(self):
        data = self.folder / "dataset.bin"
        generation = subprocess.run([sys.executable, str(GENERATOR), "--path", str(data), "--size", "16384"],
                                    capture_output=True, text=True, timeout=30)
        self.assertEqual(generation.returncode, 0, generation.stderr)
        for pattern in ("sequential", "uniform", "zipf"):
            with self.subTest(pattern=pattern):
                output = self.output(self.client(backend="sd", data=data, pattern=pattern))
                self.verify_b0(output)
                self.assertEqual(output["warmup_sd_requests"], "4")
                self.assertEqual(output["warmup_sd_read_calls"], "4")
                self.assertEqual(output["warmup_sd_read_bytes"], "16384")
                self.assertEqual(output["sd_open_direct"], "1")
                synthetic = self.output(self.client(pattern=pattern))
                for key in ("trace_hash", "checksum", "usage_resident_unique_bytes"):
                    self.assertEqual(output[key], synthetic[key], key)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2,
                  defaultTest="SdB0Tests" if SD_MODE else "B0Tests")
