"""Verify calibrated mass using the project's actual C workload generator."""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess

from calibrate_zipf import validate_config, write_json_exclusive

TOLERANCE = 0.01


def run_probe(probe, config, samples, seed):
    result = subprocess.run([str(Path(probe).resolve()), str(config["working_set_bytes"]),
                             repr(config["theta"]), str(samples), str(seed)],
                            capture_output=True, text=True, timeout=60)
    if result.returncode != 0 or "runtime error:" in result.stderr or "AddressSanitizer" in result.stderr:
        raise ValueError(f"C probe failed: {result.stderr.strip()}")
    data = json.loads(result.stdout)
    if not isinstance(data, dict):
        raise ValueError("invalid C probe output")
    for key, expected in (("working_set_bytes", config["working_set_bytes"]),
                          ("accesses", samples), ("seed", seed), ("hot_items", config["hot_items"])):
        if type(data.get(key)) is not int or data[key] != expected:
            raise ValueError(f"C probe configuration mismatch: {key}")
    if data.get("theta") != config["theta"]:
        raise ValueError("C probe theta mismatch")
    hot = data.get("hot_accesses")
    fraction = data.get("hot_access_fraction")
    if (type(hot) is not int or not 0 <= hot <= samples or
            type(fraction) not in (int, float) or not math.isfinite(fraction) or
            abs(fraction - hot / samples) > 1e-11 or
            not re.fullmatch(r"[0-9a-f]{16}", str(data.get("trace_hash", "")))):
        raise ValueError("invalid C probe observations")
    return data


def verify(config, probe, samples, seeds):
    validate_config(config)
    if type(samples) is not int or not 100000 <= samples <= 10000000:
        raise ValueError("verification needs 100000..10000000 samples per seed")
    if len(seeds) < 3 or len(set(seeds)) != len(seeds):
        raise ValueError("verification needs at least three distinct seeds")
    if any(type(seed) is not int or not 0 <= seed < 1 << 64 for seed in seeds):
        raise ValueError("seeds must be decimal uint64 values, including zero")
    observations = [run_probe(probe, config, samples, seed) for seed in seeds]
    repeat = run_probe(probe, config, samples, seeds[0])
    reproduced = repeat == observations[0]
    results = []
    for observation in observations:
        # Use the integer count, not the rounded stdout fraction.
        fraction = observation["hot_accesses"] / samples
        error = abs(fraction - config["target_access_fraction"])
        results.append({**observation, "observed_fraction": fraction,
                        "absolute_error": error, "passed": error <= TOLERANCE})
    return {"schema": "zipf-8020-verification-v1", "calibration": config,
            "probe": str(Path(probe).resolve()), "samples_per_seed": samples,
            "tolerance_absolute": TOLERANCE, "trace_reproduced": reproduced,
            "results": results, "passed": reproduced and all(row["passed"] for row in results)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--probe", default="./build/zipf_probe", type=Path)
    parser.add_argument("--samples", type=int, default=200000)
    parser.add_argument("--seeds", nargs="+", type=int, default=[42, 43, 44])
    parser.add_argument("--output", type=Path, help="new report file, never overwritten")
    args = parser.parse_args()
    try:
        with args.config.open(encoding="utf-8") as file:
            config = json.load(file)
        report = verify(config, args.probe, args.samples, args.seeds)
        if args.output:
            write_json_exclusive(args.output, report)
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2, sort_keys=True, allow_nan=False))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
