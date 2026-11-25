"""Workload checks plus seeded equivalence across backends.

Usage: test_patterns.py CLIENT SERVER [sd]. SD mode requires direct I/O.
"""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import test_tcp

CLIENT = sys.argv[1] if len(sys.argv) > 1 else "./build/client"
SERVER = sys.argv[2] if len(sys.argv) > 2 else "./build/server"
SD_MODE = len(sys.argv) > 3 and sys.argv[3] == "sd"
GENERATOR = Path(__file__).resolve().parents[1] / "scripts" / "prepare_dataset.py"


def run_client(pattern="uniform", seed=42, block=4096, capacity=8192,
               working_set=16384, accesses=4096, backend="synthetic", port=18083, data=None):
    command = [CLIENT, "--pattern", pattern, "--seed", str(seed),
               "--block-size", str(block), "--cache-size", str(capacity),
               "--working-set", str(working_set), "--accesses", str(accesses),
               "--backend", backend, "--host", "127.0.0.1", "--port", str(port)]
    if pattern == "zipf":
        command += ["--zipf-theta", "1.1"]
    if data is not None:
        command += ["--file", str(data)]
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


class OutputChecks(unittest.TestCase):
    def output(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("runtime error:", result.stderr)
        self.assertNotIn("AddressSanitizer", result.stderr)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def compare_read_results(self, first, second):
        for key in ("requests", "hits", "misses", "evictions", "load_errors",
                    "checksum", "trace_hash", "seed", "pattern", "hot20_access_fraction"):
            self.assertEqual(first[key], second[key], key)


class PatternTests(OutputChecks):
    def test_same_seed_repeats_trace_and_results(self):
        for pattern in ("sequential", "uniform", "zipf"):
            with self.subTest(pattern=pattern):
                first = self.output(run_client(pattern=pattern))
                second = self.output(run_client(pattern=pattern))
                self.compare_read_results(first, second)
                self.assertEqual(first["pattern"], pattern)

    def test_different_seeds_and_uint64_boundaries(self):
        for pattern in ("uniform", "zipf"):
            with self.subTest(pattern=pattern):
                first = self.output(run_client(pattern=pattern, seed=0))
                second = self.output(run_client(pattern=pattern, seed=18446744073709551615))
                self.assertNotEqual(first["trace_hash"], second["trace_hash"])

    def test_trace_is_independent_of_block_size(self):
        for pattern in ("uniform", "zipf"):
            checksums, hashes = [], []
            for block in (4096, 16384, 65536, 262144):
                with self.subTest(pattern=pattern, block=block):
                    output = self.output(run_client(pattern=pattern, block=block,
                        capacity=524288, working_set=1048576, accesses=10000))
                    checksums.append(output["checksum"])
                    hashes.append(output["trace_hash"])
            self.assertEqual(len(set(checksums)), 1)
            self.assertEqual(len(set(hashes)), 1)

    def test_zipf_concentrates_accesses_in_low_ranks(self):
        uniform = self.output(run_client(pattern="uniform", accesses=20000))
        zipf = self.output(run_client(pattern="zipf", accesses=20000))
        uniform_fraction = float(uniform["hot20_access_fraction"])
        zipf_fraction = float(zipf["hot20_access_fraction"])
        self.assertAlmostEqual(uniform_fraction, 0.2, delta=0.04)
        self.assertGreater(zipf_fraction, uniform_fraction + 0.25)

    def test_invalid_options_report_their_cause(self):
        cases = [
            (["--pattern", "other"], "Invalid pattern"),
            (["--seed", "-1"], "Invalid seed"),
            (["--seed", "18446744073709551616"], "Invalid seed"),
            (["--seed"], "Invalid seed"),
            (["--pattern", "zipf", "--zipf-theta", "nan"], "Invalid Zipf theta"),
            (["--pattern", "zipf", "--zipf-theta", "inf"], "Invalid Zipf theta"),
            (["--pattern", "zipf", "--zipf-theta", "0"], "Invalid Zipf theta"),
            (["--pattern", "zipf", "--zipf-theta", "2.1"], "Invalid Zipf theta"),
            (["--zipf-theta", "1.1"], "requires --pattern zipf"),
        ]
        for args, expected in cases:
            with self.subTest(args=args):
                result = subprocess.run([CLIENT, *args], capture_output=True,
                                        text=True, timeout=10)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(expected, result.stderr)

    def test_tcp_preserves_seeded_loads(self):
        test_tcp.SERVER = SERVER
        harness = test_tcp.TcpTests()
        for pattern in ("uniform", "zipf"):
            with self.subTest(pattern=pattern), harness.server() as port:
                synthetic = self.output(run_client(pattern=pattern, accesses=2000))
                tcp = self.output(run_client(pattern=pattern, accesses=2000,
                                             backend="tcp", port=port))
                self.compare_read_results(synthetic, tcp)
                self.assertEqual(tcp["tcp_requests"], tcp["misses"])


class SdPatternTests(OutputChecks):
    def compare_sd(self, pattern):
        Path("build").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="patterns-sd-", dir="build") as directory:
            data = Path(directory) / "dataset.bin"
            preparation = subprocess.run([sys.executable, str(GENERATOR),
                "--path", str(data), "--size", "16384"],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(preparation.returncode, 0, preparation.stderr)
            synthetic = self.output(run_client(pattern=pattern, accesses=2000))
            sd = self.output(run_client(pattern=pattern, accesses=2000, backend="sd", data=data))
            self.compare_read_results(synthetic, sd)
            self.assertEqual(sd["sd_open_direct"], "1")
            self.assertEqual(sd["sd_requests"], sd["misses"])

    def test_uniform_sd_matches_synthetic(self):
        self.compare_sd("uniform")

    def test_zipf_sd_matches_synthetic(self):
        self.compare_sd("zipf")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2,
                  defaultTest="SdPatternTests" if SD_MODE else "PatternTests")
