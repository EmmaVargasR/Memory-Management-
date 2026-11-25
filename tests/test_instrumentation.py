"""Validate measured summaries against raw CSV; never assert a speed threshold."""
from decimal import Decimal
from pathlib import Path
import csv
import subprocess
import sys
import tempfile
import unittest

import test_tcp

CLIENT = sys.argv[1] if len(sys.argv) > 1 else "./build/client"
SERVER = sys.argv[2] if len(sys.argv) > 2 else "./build/server"
SD_MODE = len(sys.argv) > 3 and sys.argv[3] == "sd"
GENERATOR = Path(__file__).resolve().parents[1] / "scripts" / "prepare_dataset.py"


class Common(unittest.TestCase):
    def setUp(self):
        Path("build").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="metrics-", dir="build")
        self.addCleanup(self.directory.cleanup)
        self.folder = Path(self.directory.name)

    def client(self, backend="synthetic", pattern="sequential", block=4096,
               capacity=8192, working_set=16384, accesses=128, extra=(), port=18083, data=None):
        command = [CLIENT, "--backend", backend, "--pattern", pattern, "--seed", "42",
                   "--block-size", str(block), "--cache-size", str(capacity),
                   "--working-set", str(working_set), "--accesses", str(accesses),
                   "--port", str(port)]
        if data is not None:
            command += ["--file", str(data)]
        return subprocess.run([*command, *extra], capture_output=True, text=True, timeout=40)

    def output(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("runtime error:", result.stderr)
        self.assertNotIn("AddressSanitizer", result.stderr)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def verify_csv(self, result, path):
        output = self.output(result)
        with path.open(newline="") as file:
            rows = list(csv.DictReader(file))
        self.assertEqual(len(rows), int(output["requests"]))
        self.assertEqual([int(row["access_index"]) for row in rows], list(range(len(rows))))
        for row in rows:
            self.assertIn(row["was_hit"], ("0", "1"))
            self.assertGreaterEqual(int(row["access_ns"]), int(row["backend_ns"]))
            if row["was_hit"] == "1":
                self.assertEqual(int(row["backend_ns"]), 0)
        for group in ("all", "hit", "miss", "backend"):
            selected = [row for row in rows if group == "all" or
                        (row["was_hit"] == "1") == (group == "hit")]
            key = "backend_ns" if group == "backend" else "access_ns"
            values = sorted(int(row[key]) for row in selected)
            prefix = f"latency_{group}_"
            self.assertEqual(int(output[prefix + "count"]), len(values))
            if not values:
                for metric in ("sum_ns", "mean_ns", "min_ns", "p50_ns", "p95_ns", "p99_ns", "max_ns"):
                    self.assertEqual(output[prefix + metric], "na")
                continue
            self.assertEqual(Decimal(output[prefix + "sum_ns"]), sum(values))
            expected = Decimal(sum(values)) / len(values)
            self.assertLessEqual(abs(Decimal(output[prefix + "mean_ns"]) - expected), Decimal("0.00051"))
            self.assertEqual(int(output[prefix + "min_ns"]), values[0])
            self.assertEqual(int(output[prefix + "max_ns"]), values[-1])
            for percent in (50, 95, 99):
                rank = (len(values) * percent + 99) // 100
                self.assertEqual(int(output[prefix + f"p{percent}_ns"]), values[rank - 1])
        self.assertEqual(int(output["latency_hit_count"]), int(output["hits"]))
        self.assertEqual(int(output["latency_miss_count"]), int(output["misses"]))
        self.assertEqual(output["latency_backend_count"], output["misses"])
        self.assertEqual(output["usage_evicted_blocks"], output["evictions"])
        self.assertEqual(int(output["usage_evicted_blocks"]) + int(output["usage_resident_blocks"]),
                         int(output["misses"]))
        self.assertLessEqual(Decimal(output["latency_all_sum_ns"]),
                             Decimal(output["elapsed_seconds"]) * 1000000000 + 2)
        return output

    def compare_behavior(self, first, second):
        for key in ("requests", "hits", "misses", "evictions", "load_errors", "checksum", "trace_hash"):
            self.assertEqual(first[key], second[key], key)


class InstrumentationTests(Common):
    def test_csv_matches_summaries_for_all_patterns(self):
        for pattern in ("sequential", "uniform", "zipf"):
            with self.subTest(pattern=pattern):
                path = self.folder / f"{pattern}.csv"
                self.verify_csv(self.client(pattern=pattern, accesses=2048,
                                            extra=["--latency-csv", str(path)]), path)

    def test_known_utilization_and_partial_residency(self):
        output = self.output(self.client(block=16, capacity=16, working_set=32, accesses=3))
        self.assertEqual(output["usage_evicted_blocks"], "1")
        self.assertEqual(output["usage_evicted_unique_bytes"], "16")
        self.assertEqual(output["usage_resident_blocks"], "1")
        self.assertEqual(output["usage_resident_unique_bytes"], "8")
        self.assertEqual(float(output["block_utilization_evicted"]), 1.0)
        self.assertEqual(float(output["block_utilization_resident"]), 0.5)
        self.assertEqual(float(output["block_utilization_all_loaded"]), 0.75)

    def test_repeated_word_and_empty_eviction_group(self):
        output = self.output(self.client(block=16, capacity=16, working_set=16, accesses=1))
        self.assertEqual(output["latency_hit_count"], "0")
        self.assertEqual(output["latency_hit_mean_ns"], "na")
        self.assertEqual(output["block_utilization_evicted"], "na")
        output = self.output(self.client(block=8, capacity=8, working_set=8, accesses=1000))
        self.assertEqual(output["usage_resident_unique_bytes"], "8")
        self.assertEqual(output["misses"], "1")

    def test_metrics_off_preserves_behavior(self):
        for pattern in ("sequential", "uniform", "zipf"):
            with self.subTest(pattern=pattern):
                enabled = self.output(self.client(pattern=pattern, accesses=2048))
                disabled = self.output(self.client(pattern=pattern, accesses=2048,
                                                  extra=["--metrics", "off"]))
                self.compare_behavior(enabled, disabled)
                self.assertEqual(disabled["metrics_enabled"], "0")
                self.assertNotIn("latency_all_count", disabled)

    def test_csv_does_not_overwrite_existing_file(self):
        path = self.folder / "existing.csv"
        path.write_text("preserve this\n")
        result = self.client(extra=["--latency-csv", str(path)])
        self.assertEqual(result.returncode, 1)
        self.assertIn("not overwritten", result.stderr)
        self.assertEqual(path.read_text(), "preserve this\n")

    def test_invalid_metrics_options(self):
        for extra, message in ((["--metrics", "maybe"], "Invalid metrics mode"),
                               (["--metrics"], "Invalid metrics mode"),
                               (["--latency-csv"], "requires a new file path"),
                               (["--metrics", "off", "--latency-csv", "unused.csv"], "requires --metrics on")):
            with self.subTest(extra=extra):
                result = self.client(extra=extra)
                self.assertEqual(result.returncode, 1)
                self.assertIn(message, result.stderr)

    def test_tcp_usage_matches_synthetic_and_csv_is_consistent(self):
        test_tcp.SERVER = SERVER
        harness = test_tcp.TcpTests()
        for pattern in ("uniform", "zipf"):
            with self.subTest(pattern=pattern), harness.server() as port:
                path = self.folder / f"tcp-{pattern}.csv"
                remote = self.verify_csv(self.client(backend="tcp", pattern=pattern, port=port,
                    accesses=2048, extra=["--latency-csv", str(path)]), path)
                local = self.output(self.client(pattern=pattern, accesses=2048))
                self.compare_behavior(local, remote)
                for key in local:
                    if key.startswith("usage_") or key.startswith("block_utilization_"):
                        self.assertEqual(local[key], remote[key], key)


class SdInstrumentationTests(Common):
    def test_sd_summaries_and_utilization_for_all_patterns(self):
        data = self.folder / "dataset.bin"
        result = subprocess.run([sys.executable, str(GENERATOR), "--path", str(data), "--size", "16384"],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        for pattern in ("sequential", "uniform", "zipf"):
            with self.subTest(pattern=pattern):
                path = self.folder / f"sd-{pattern}.csv"
                direct = self.verify_csv(self.client(backend="sd", data=data, pattern=pattern,
                    accesses=2048, extra=["--latency-csv", str(path)]), path)
                synthetic = self.output(self.client(pattern=pattern, accesses=2048))
                self.compare_behavior(synthetic, direct)
                self.assertEqual(direct["sd_open_direct"], "1")
                self.assertEqual(direct["sd_requests"], direct["latency_backend_count"])
                for key in synthetic:
                    if key.startswith("usage_") or key.startswith("block_utilization_"):
                        self.assertEqual(synthetic[key], direct[key], key)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2,
                  defaultTest="SdInstrumentationTests" if SD_MODE else "InstrumentationTests")
