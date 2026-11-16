"""Deterministic integration checks; no timing thresholds or dependencies."""
import subprocess
import sys
import unittest

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./build/client"


class ClientTests(unittest.TestCase):
    def run_case(self, block, capacity, working_set, accesses):
        result = subprocess.run(
            [BINARY, "--block-size", str(block), "--cache-size", str(capacity),
             "--working-set", str(working_set), "--accesses", str(accesses)],
            capture_output=True, text=True, check=True)
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def check_counts(self, output, requests, hits, misses, evictions):
        for key, value in {"requests": requests, "hits": hits, "misses": misses,
                           "evictions": evictions, "load_errors": 0}.items():
            self.assertEqual(int(output[key]), value, key)

    def test_two_passes_with_evictions(self):
        output = self.run_case(4096, 8192, 16384, 4096)
        self.check_counts(output, 4096, 4088, 8, 6)
        expected = sum((i % 16384) % 251 for i in range(4096 * 8))
        self.assertEqual(int(output["checksum"]), expected)

    def test_full_cache_still_has_cold_misses(self):
        output = self.run_case(4096, 16384, 16384, 4096)
        self.check_counts(output, 4096, 4092, 4, 0)

    def test_bytes_are_independent_of_block_size(self):
        checksums = []
        for block in (4096, 16384, 65536, 262144):
            output = self.run_case(block, 524288, 1048576, 10000)
            checksums.append(int(output["checksum"]))
        self.assertEqual(checksums, [sum(i % 251 for i in range(80000))] * 4)

    def test_single_slot_and_wraparound(self):
        output = self.run_case(16, 16, 32, 8)
        self.check_counts(output, 8, 4, 4, 3)
        self.assertEqual(int(output["checksum"]), sum(i % 32 for i in range(64)))

    def test_invalid_arguments(self):
        cases = [
            ["--accesses", "0"], ["--accesses", "-1"],
            ["--accesses", "18446744073709551616"],
            ["--block-size", "0"], ["--block-size", "7"],
            ["--cache-size", "4097"], ["--working-set", "17"],
            ["--accesses"], ["--unknown", "1"],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = subprocess.run([BINARY, *arguments], capture_output=True, text=True)
                self.assertEqual(result.returncode, 1, result.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
