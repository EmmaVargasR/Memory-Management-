"""Calibrate Zipf probability mass, independently of seed, B and backend."""
import argparse
import json
import math
from pathlib import Path

SCHEMA = "zipf-8020-v1"
METHOD = "euler-maclaurin-prefix4096-order8-bisection-v1"
TARGET = 0.8
PREFIX = 4096
MAX_ITEMS = 1 << 48


def population(working_set):
    if type(working_set) is not int or working_set < 8 or working_set % 8:
        raise ValueError("working-set must be positive bytes divisible by 8")
    items = working_set // 8
    if items > MAX_ITEMS:
        raise ValueError("Zipf supports at most 2^48 logical words")
    return items, (items + 4) // 5


def harmonic(items, theta):
    """H_n(theta), constant memory; exact direct sum for n<=4096.

    Large n: Euler-Maclaurin tail after an explicitly summed prefix. Four
    corrections include derivatives through order 7. At prefix 4096 and
    theta in [0,2], the next term is far below double precision at these
    scales. Numerical accuracy is tested against independent direct sums.
    """
    if type(items) is not int or not 1 <= items <= MAX_ITEMS:
        raise ValueError("invalid item count")
    if not math.isfinite(theta) or not 0 <= theta <= 2:
        raise ValueError("theta must be finite in [0,2]")
    if theta == 0:
        return float(items)
    first = min(items, PREFIX)
    result = math.fsum(k ** -theta for k in range(1, first + 1))
    if items == first:
        return result
    a, b = float(first), float(items)
    exponent = 1.0 - theta
    log_ratio = math.log(b / a)
    integral = log_ratio if exponent == 0 else a ** exponent * math.expm1(exponent * log_ratio) / exponent
    corrections = [integral, (b ** -theta - a ** -theta) / 2]
    for order, coefficient in ((1, 1 / 12), (3, -1 / 720),
                               (5, 1 / 30240), (7, -1 / 1209600)):
        rising = math.prod(theta + j for j in range(order))
        corrections.append(coefficient * rising * (a ** (-theta - order) - b ** (-theta - order)))
    return math.fsum([result, *corrections])


def hot_probability(items, hot_items, theta):
    if not 1 <= hot_items <= items:
        raise ValueError("invalid hot subset")
    return harmonic(hot_items, theta) / harmonic(items, theta)


def calibrate(working_set):
    items, hot_items = population(working_set)
    lower = hot_items / items
    upper = hot_probability(items, hot_items, 2.0)
    if not lower < TARGET or upper < TARGET - 1e-14:
        raise ValueError("80/20 is unattainable for this population with 0 < theta <= 2; use a larger working-set")
    left, right = 0.0, 2.0
    for _ in range(60):
        middle = (left + right) / 2
        if hot_probability(items, hot_items, middle) < TARGET:
            left = middle
        else:
            right = middle
    theta = (left + right) / 2
    expected = hot_probability(items, hot_items, theta)
    if abs(expected - TARGET) > 1e-12:
        raise ValueError("calibration did not converge")
    return {"schema": SCHEMA, "working_set_bytes": working_set, "read_bytes": 8,
            "items": items, "hot_items": hot_items, "hot_fraction_effective": hot_items / items,
            "target_access_fraction": TARGET, "theta": theta,
            "expected_hot_access_fraction": expected, "method": METHOD,
            "rank_mapping": "rank-k-to-address-(k-1)*8",
            "sampler": "integral-envelope-v1", "prng": "splitmix64-v1"}


def validate_config(config):
    if not isinstance(config, dict) or config.get("schema") != SCHEMA:
        raise ValueError("unsupported calibration schema")
    items, hot_items = population(config.get("working_set_bytes"))
    for key, expected in (("read_bytes", 8), ("items", items), ("hot_items", hot_items)):
        if type(config.get(key)) is not int or config[key] != expected:
            raise ValueError(f"invalid calibration field: {key}")
    for key, expected in (("method", METHOD), ("rank_mapping", "rank-k-to-address-(k-1)*8"),
                          ("sampler", "integral-envelope-v1"), ("prng", "splitmix64-v1")):
        if config.get(key) != expected:
            raise ValueError(f"incompatible calibration field: {key}")
    for key in ("theta", "target_access_fraction", "expected_hot_access_fraction", "hot_fraction_effective"):
        value = config.get(key)
        if type(value) not in (int, float) or not math.isfinite(value):
            raise ValueError(f"invalid calibration field: {key}")
    if not 0 < config["theta"] <= 2 or config["target_access_fraction"] != TARGET:
        raise ValueError("invalid calibrated theta or target")
    probability = hot_probability(items, hot_items, config["theta"])
    if (abs(probability - TARGET) > 1e-11 or
            abs(probability - config["expected_hot_access_fraction"]) > 1e-11 or
            abs(config["hot_fraction_effective"] - hot_items / items) > 1e-14):
        raise ValueError("calibration probability does not match 80/20")
    return config


def write_json_exclusive(path, data):
    """Never overwrite prior experiment artifacts."""
    created = False
    try:
        with Path(path).open("x", encoding="utf-8", newline="\n") as file:
            created = True
            json.dump(data, file, indent=2, sort_keys=True, allow_nan=False)
            file.write("\n")
    except BaseException:
        if created:
            Path(path).unlink(missing_ok=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--working-set", required=True, type=int, help="bytes; logical words are 8 bytes")
    parser.add_argument("--output", type=Path, help="new JSON file, never overwritten")
    args = parser.parse_args()
    try:
        config = calibrate(args.working_set)
        if args.output:
            write_json_exclusive(args.output, config)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps(config, indent=2, sort_keys=True, allow_nan=False))


if __name__ == "__main__":
    main()
