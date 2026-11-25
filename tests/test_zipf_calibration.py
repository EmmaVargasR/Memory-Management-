"""Theory checked by independent direct sums; C probe checked against client."""
from copy import deepcopy
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import calibrate_zipf as calibration
import verify_zipf as verification

MATH_ONLY = len(sys.argv) > 1 and sys.argv[1] == "--math-only"
PROBE = sys.argv[1] if len(sys.argv) > 1 and not MATH_ONLY else "./build/zipf_probe"
CLIENT = sys.argv[2] if len(sys.argv) > 2 else "./build/client"


class MathTests(unittest.TestCase):
    def test_euler_maclaurin_against_independent_direct_sum(self):
        for items in (1, 17, 4095, 4096, 4097, 20000, 100000):
            for theta in (0, 0.01, 0.5, 0.9, 1, 1 + 1e-12, 1.1, 2):
                with self.subTest(items=items, theta=theta):
                    reference = math.fsum(math.exp(-theta * math.log(k)) for k in range(1, items + 1))
                    self.assertTrue(math.isclose(calibration.harmonic(items, theta), reference,
                                                 rel_tol=3e-13, abs_tol=3e-13))

    def test_calibration_matches_direct_probability(self):
        for working_set in (128, 16384, 80000):
            config = calibration.calibrate(working_set)
            theta, items, hot = config["theta"], config["items"], config["hot_items"]
            probability = (math.fsum(k ** -theta for k in range(1, hot + 1)) /
                           math.fsum(k ** -theta for k in range(1, items + 1)))
            self.assertAlmostEqual(probability, 0.8, places=11)
            self.assertEqual(config, calibration.validate_config(config))

    def test_large_populations_and_block_independent_parameters(self):
        for working_set in (268435456, 536870912, 8 * (1 << 48)):
            config = calibration.calibrate(working_set)
            self.assertLess(abs(config["expected_hot_access_fraction"] - 0.8), 1e-12)
            self.assertEqual(config, calibration.validate_config(config))
            self.assertNotIn("block_size", config)
            self.assertNotIn("seed", config)

    def test_invalid_or_unattainable_population(self):
        for working_set in (0, -8, 17, True, 8, 24, 40, 8 * ((1 << 48) + 1)):
            with self.subTest(working_set=working_set), self.assertRaises(ValueError):
                calibration.calibrate(working_set)

    def test_tampered_config_is_rejected(self):
        original = calibration.calibrate(16384)
        for key, value in (("theta", 1.1), ("target_access_fraction", 0.9),
                           ("expected_hot_access_fraction", 0.2), ("items", 12),
                           ("schema", "other"), ("read_bytes", True), ("theta", float("nan"))):
            changed = deepcopy(original)
            changed[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                calibration.validate_config(changed)

    def test_verifier_rejects_weak_or_invalid_sampling(self):
        config = calibration.calibrate(16384)
        for samples, seeds in ((10, [42, 43, 44]), (100000, [42]),
                               (100000, [42, 42, 43]), (100000, [-1, 42, 43]),
                               (100000, [42, 43, 1 << 64])):
            with self.subTest(samples=samples, seeds=seeds), self.assertRaises(ValueError):
                verification.verify(config, "unused-probe", samples, seeds)


class CalibrationCliTests(unittest.TestCase):
    def test_json_file_creation_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "calibration.json"
            command = [sys.executable, str(ROOT / "scripts/calibrate_zipf.py"),
                       "--working-set", "16384", "--output", str(path)]
            first = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(first.returncode, 0, first.stderr)
            original = path.read_bytes()
            config = json.loads(original)
            self.assertEqual(json.loads(first.stdout), calibration.validate_config(config))
            second = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(path.read_bytes(), original)


class ProbeTests(unittest.TestCase):
    def test_three_seeds_and_repeated_trace_at_two_scales(self):
        for working_set in (16384, 268435456):
            with self.subTest(working_set=working_set):
                report = verification.verify(calibration.calibrate(working_set), PROBE,
                                             200000, [0, 42, 18446744073709551615])
                self.assertTrue(report["passed"], report)
                self.assertTrue(report["trace_reproduced"])
                self.assertEqual(len(report["results"]), 3)

    def test_probe_is_the_same_generator_used_by_client(self):
        config = calibration.calibrate(16384)
        for block in (4096, 8192):
            with self.subTest(block=block):
                probe = verification.run_probe(PROBE, config, 4096, 42)
                result = subprocess.run([CLIENT, "--backend", "synthetic", "--pattern", "zipf",
                    "--zipf-theta", repr(config["theta"]), "--seed", "42", "--block-size", str(block),
                    "--cache-size", "8192", "--working-set", "16384", "--accesses", "4096"],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn("runtime error:", result.stderr)
                output = dict(line.split("=", 1) for line in result.stdout.splitlines())
                self.assertEqual(probe["trace_hash"], output["trace_hash"])
                self.assertLess(abs(probe["hot_accesses"] / 4096 - float(output["hot20_access_fraction"])), 1e-9)

    def test_invalid_probe_arguments(self):
        for args in ([], ["16384", "nan", "10", "42"], ["17", "1", "10", "42"],
                     ["16384", "1", "0", "42"], ["16384", "1", "10", "-1"],
                     ["16384", "1", "10", "18446744073709551616"]):
            result = subprocess.run([PROBE, *args], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2,
                  defaultTest=["MathTests", "CalibrationCliTests"] if MATH_ONLY else
                              ["MathTests", "CalibrationCliTests", "ProbeTests"])
