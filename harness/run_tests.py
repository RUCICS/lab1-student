#!/usr/bin/env python3
"""Run supported workload tests and report pass/fail.

This runner automatically skips workloads that use actions not supported
by the basic harness (e.g. resize, log, pipeout, capture, layout).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

from harness.runner import Runner, load_workload


def main():
    parser = argparse.ArgumentParser(description="Run mini-tmux public workload tests")
    parser.add_argument("--binary", required=True, help="Path to mini-tmux binary")
    parser.add_argument("--workloads", required=True, help="Directory containing YAML workload files")
    parser.add_argument("--report", default=None, help="Path to write JSON report")
    args = parser.parse_args()

    workloads_dir = Path(args.workloads)
    yaml_files = sorted(workloads_dir.glob("*.yaml"))
    if not yaml_files:
        print(f"No workload files found in {workloads_dir}")
        sys.exit(1)

    runner = Runner(binary=args.binary, use_tmux=False)
    results = []
    skipped = []

    for yf in yaml_files:
        workload = load_workload(yf)
        wid = workload["id"]
        wname = workload["name"]

        if not runner.can_run(workload):
            skipped.append(wname)
            continue

        print(f"Running: {wid} ({wname})...", end=" ", flush=True)
        result = runner.run_workload(workload)

        msg = ""
        if not result.passed:
            if result.error:
                msg = f" ({result.error})"
            else:
                failed = next((s for s in result.steps if not s.passed), None)
                if failed:
                    msg = f" (failed at: {failed.action}: {failed.message})"

        status = "PASS" if result.passed else "FAIL"
        print(f"{status}{msg}")

        results.append({
            "name": wname,
            "id": wid,
            "passed": result.passed,
            "error": result.error,
            "failed_step": next(
                (s.action for s in result.steps if not s.passed), None
            ),
        })

    passed = sum(1 for r in results if r["passed"])
    total = len(results)

    print()
    if skipped:
        print(f"Skipped {len(skipped)} unsupported workloads: {', '.join(skipped)}")
    print(f"Passed {passed}/{total} basic tests")

    report = {
        "pass_count": passed,
        "total_count": total,
        "workloads": results,
    }
    if args.report:
        with open(args.report, "w") as f:
            json.dump(report, f, indent=2)
        print(f"Report written to: {args.report}")

    sys.exit(0 if passed == total else 1)


if __name__ == "__main__":
    main()
