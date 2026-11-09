#!/usr/bin/env python3
"""Section 4.3.3: retain logs grouped by workload, then policy and repetition."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from qemu_tools import ROOT, EVIDENCE, POLICIES, WORKLOADS, Session, build, user_console


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--resume", action='store_true')
    args = parser.parse_args()
    if args.runs < 1 or args.jobs < 1:
        parser.error("--runs and --jobs must be positive")
    for workload in WORKLOADS:
        for policy in POLICIES:
            build(policy)
            output = EVIDENCE / "runner_logs" / workload / policy
            output.mkdir(parents=True, exist_ok=True)
            def execute(run):
                path = output / f"run-{run:02d}.log"
                if args.resume and path.exists():
                    text = path.read_text()
                    if (f'DONE|{workload}' in user_console(text) and 'QEMU: Terminated' in text
                            and 'panic:' not in text and 'FAIL:' not in user_console(text)):
                        print(f"PASS: retained {workload} {policy} run {run}/{args.runs}", flush=True)
                        return
                session = Session(policy, path)
                try:
                    session.run(workload, f"DONE|{workload}")
                finally:
                    session.close()
                text = path.read_text()
                if "panic:" in text or "FAIL:" in user_console(text):
                    raise AssertionError(f"Failed workload: {path}")
                print(f"PASS: {workload} {policy} run {run}/{args.runs}", flush=True)
            # Section 4.3.3: optional parallel repetitions stay grouped by program.
            # QEMU snapshot overlays isolate each guest's filesystem writes.
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                list(pool.map(execute, range(1, args.runs + 1)))
    # Sections 2.5, 3.6, 4.3: the required runner also checks group behavior.
    from validate_workloads import main as validate
    validate()


if __name__ == "__main__":
    main()
