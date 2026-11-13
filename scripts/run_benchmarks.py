import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MAPS = ["linear", "chain", "std", "sparse"]
DATASETS = ["u64", "string-person"]
DISTRIBUTIONS = ["sequential", "shuffled", "random"]


def main():
    parser = argparse.ArgumentParser(description="Compare the four maps using the same input")
    parser.add_argument("--count", type=int, default=200000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--operation", choices=["insert", "find-hit", "find-miss"], default="insert")
    parser.add_argument("--mode", choices=["reserved", "growing"], default="reserved")
    parser.add_argument("--hash", choices=["mixed", "weak"], default="mixed")
    parser.add_argument("--output", type=Path, default=ROOT / "results" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ"))
    args = parser.parse_args()
    binary = ROOT / "build/release/hashmap_bench"
    if not binary.is_file():
        parser.error("build the release preset first")
    if args.count < 1 or args.repeats < 1 or not 0 <= args.seed < 2**64:
        parser.error("count and repeats must be positive, and seed must fit uint64")
    if args.output.exists():
        parser.error("output directory already exists")
    args.output.mkdir(parents=True)
    metadata = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(), "arguments": vars(args).copy(),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "status": "running",
    }
    metadata["arguments"]["output"] = str(args.output)
    commands = binary.parent / "compile_commands.json"
    if commands.exists():
        metadata["compile_commands"] = json.loads(commands.read_text())
    metadata_path = args.output / "metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    cases = [(name, dataset, distribution) for name in MAPS
             for dataset in DATASETS for distribution in DISTRIBUTIONS]
    results, inputs = {}, {}
    order = random.Random(2026)
    try:
        with (args.output / "trials.jsonl").open("w") as raw:
            for trial in range(args.repeats + 1):
                order.shuffle(cases)
                for name, dataset, distribution in cases:
                    command = [str(binary), "--map", name, "--dataset", dataset,
                               "--distribution", distribution, "--count", str(args.count),
                               "--seed", str(args.seed), "--operation", args.operation,
                               "--mode", args.mode, "--hash", args.hash, "--diagnostics"]
                    completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
                    if completed.returncode:
                        raise RuntimeError(completed.stderr.strip())
                    row = json.loads(completed.stdout)
                    key = (dataset, distribution)
                    signature = (row["fingerprint"], row["checksum"])
                    if inputs.setdefault(key, signature) != signature:
                        raise RuntimeError("maps used different inputs or produced different results")
                    if trial == 0:
                        continue
                    row.update(trial=trial, command=command)
                    raw.write(json.dumps(row) + "\n")
                    raw.flush()
                    results.setdefault((name, dataset, distribution), []).append(row)
                print("Warmup complete" if trial == 0 else f"Trial {trial}/{args.repeats} complete")
        with (args.output / "summary.csv").open("w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["map", "dataset", "distribution", "count", "seed", "operation", "mode", "hash",
                             "actual_load", "final_capacity", "median_ns_per_op", "min_ns_per_op", "max_ns_per_op"])
            for (name, dataset, distribution), runs in results.items():
                times = [run["ns_per_op"] for run in runs]
                writer.writerow([name, dataset, distribution, args.count, args.seed, args.operation, args.mode,
                                 args.hash, runs[0]["actual_load"], runs[0]["final_capacity"],
                                 statistics.median(times), min(times), max(times)])
        metadata["status"] = "complete"
    except (RuntimeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        metadata.update(status="failed", error=str(error))
        print(error, file=sys.stderr)
    finally:
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
    print(args.output.resolve())
    return 0 if metadata["status"] == "complete" else 1


if __name__ == "__main__":
    raise SystemExit(main())
