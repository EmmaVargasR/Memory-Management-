"""Create a new regular dataset file: byte at absolute address A is A % 251."""
import argparse
import hashlib
import os
from pathlib import Path


def positive_aligned_size(text):
    if not text.isascii() or not text.isdecimal():
        raise argparse.ArgumentTypeError("size must be decimal bytes")
    size = int(text)
    if size <= 0 or size % 4096 != 0:
        raise argparse.ArgumentTypeError("size must be positive and divisible by 4096")
    return size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", type=Path, required=True)
    parser.add_argument("--size", type=positive_aligned_size, required=True)
    args = parser.parse_args()
    args.path.parent.mkdir(parents=True, exist_ok=True)
    chunk = bytes(range(251)) * 4096  # Multiple of the pattern period.
    digest = hashlib.sha256()
    try:
        # Exclusive create refuses to overwrite existing files or device paths.
        with args.path.open("xb") as stream:
            remaining = args.size
            while remaining:
                data = chunk[:min(remaining, len(chunk))]
                stream.write(data)
                digest.update(data)
                remaining -= len(data)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as error:
        parser.exit(1, f"Dataset creation failed: {error}\n")
    print(f"dataset_path={args.path}")
    print(f"dataset_bytes={args.size}")
    print(f"dataset_sha256={digest.hexdigest()}")


if __name__ == "__main__":
    main()
