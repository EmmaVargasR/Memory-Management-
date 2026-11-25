"""Direct-I/O integration tests. Temp files live in build/ on the target filesystem.

No skips or buffered fallback: unavailable STATX_DIOALIGN/O_DIRECT is an error.
"""
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
import unittest

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./build/client"
GENERATOR = Path(__file__).resolve().parents[1] / "scripts" / "prepare_dataset.py"


class SdTests(unittest.TestCase):
    def setUp(self):
        Path("build").mkdir(exist_ok=True)
        self.directory = tempfile.TemporaryDirectory(prefix="sd-test-", dir="build")
        self.addCleanup(self.directory.cleanup)
        self.data = Path(self.directory.name) / "dataset.bin"

    def generate(self, size=16384):
        result = subprocess.run(
            [sys.executable, str(GENERATOR), "--path", str(self.data), "--size", str(size)],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def client(self, block=4096, capacity=8192, working_set=16384,
               accesses=4096, backend="sd", include_file=True):
        command = [BINARY, "--backend", backend, "--block-size", str(block),
                   "--cache-size", str(capacity), "--working-set", str(working_set),
                   "--accesses", str(accesses)]
        if include_file:
            command += ["--file", str(self.data)]
        return subprocess.run(command, capture_output=True, text=True, timeout=30)

    def output(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("runtime error:", result.stderr)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def test_generator_content_and_no_overwrite(self):
        output = self.generate(1052672)  # Cross a generator chunk boundary.
        data = self.data.read_bytes()
        self.assertEqual(data, bytes(i % 251 for i in range(1052672)))
        self.assertEqual(output["dataset_sha256"], hashlib.sha256(data).hexdigest())
        result = subprocess.run(
            [sys.executable, str(GENERATOR), "--path", str(self.data), "--size", "4096"],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.data.read_bytes(), data)

    def test_same_results_as_synthetic_for_four_block_sizes(self):
        self.generate(1048576)
        for block in (4096, 16384, 65536, 262144):
            with self.subTest(block=block):
                args = dict(block=block, capacity=524288, working_set=1048576, accesses=262144)
                direct = self.output(self.client(**args))
                synthetic = self.output(self.client(backend="synthetic", include_file=False, **args))
                for key in ("requests", "hits", "misses", "evictions", "load_errors", "checksum"):
                    self.assertEqual(direct[key], synthetic[key], key)
                self.assertEqual(direct["sd_open_direct"], "1")
                self.assertEqual(direct["sd_alignment_source"], "statx")
                self.assertGreater(int(direct["sd_memory_alignment_bytes"]), 0)
                self.assertGreater(int(direct["sd_offset_alignment_bytes"]), 0)
                self.assertEqual(int(direct["sd_requests"]), int(direct["misses"]))
                self.assertGreaterEqual(int(direct["sd_read_calls"]), int(direct["misses"]))
                self.assertEqual(int(direct["sd_read_bytes"]), int(direct["misses"]) * block)
                self.assertEqual(direct["tcp_connections"], "0")

    def test_full_cache_has_four_cold_misses(self):
        self.generate()
        result = self.output(self.client(capacity=16384))
        self.assertEqual(result["misses"], "4")
        self.assertEqual(result["hits"], "4092")
        self.assertEqual(result["evictions"], "0")
        self.assertEqual(result["sd_requests"], "4")
        self.assertEqual(result["sd_read_bytes"], "16384")
        self.assertEqual(result["checksum"], "4083442")

    def test_missing_file_is_an_error(self):
        result = self.client()
        self.assertEqual(result.returncode, 1)
        self.assertIn("SD open O_DIRECT", result.stderr)

    def test_wrong_file_length_is_rejected(self):
        self.generate(4096)
        result = self.client()
        self.assertEqual(result.returncode, 1)
        self.assertIn("exactly matches", result.stderr)

    def test_small_block_is_rejected(self):
        self.generate()
        result = self.client(block=16)
        self.assertEqual(result.returncode, 1)
        self.assertIn("SD block size", result.stderr)

    def test_sd_needs_file_and_other_backends_reject_file(self):
        result = self.client(include_file=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("--file is required", result.stderr)
        result = self.client(backend="synthetic")
        self.assertEqual(result.returncode, 1)
        self.assertIn("--file requires --backend sd", result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
