#!/usr/bin/env python3
"""Section 4.3.4: nine plots from actual lifecycle and scheduling evidence."""
import csv
from pathlib import Path
import os
import re
import statistics
os.environ.setdefault('MPLCONFIGDIR', str(Path(os.environ.get("CS140_EVIDENCE_DIR", str(Path(__file__).resolve().parent))) / "verification/matplotlib"))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from tests.qemu_tools import ROOT, EVIDENCE, POLICIES, WORKLOADS, user_console


def metrics(path):
    text = path.read_text()
    root = re.search(r"WORKLOAD\|\w+\|(\d+)", user_console(text))
    if not root:
        raise ValueError(f"No workload root: {path}")
    root = int(root[1])
    created, parents, exited, dispatched = {}, {}, {}, {}
    dispatches = []
    for kind, pid, tick, value in re.findall(r"TRACE\|(CREATE|EXIT|DISPATCH)\|(\d+)\|(\d+)\|(-?\d+)", text):
        pid, tick, value = int(pid), int(tick), int(value)
        if kind == "CREATE":
            created[pid], parents[pid] = tick, value
        elif kind == "EXIT":
            if value != 0 and pid >= root:
                raise ValueError(f"Worker failure pid={pid} status={value}: {path}")
            exited[pid] = tick
        else:
            dispatched.setdefault(pid, tick)
            dispatches.append((pid, tick))
    family = {root}
    while True:
        expanded = family | {p for p, parent in parents.items() if parent in family}
        if expanded == family:
            break
        family = expanded
    if any(p not in created or p not in dispatched or p not in exited for p in family):
        raise ValueError(f"Incomplete lifecycle: {path}")
    # Each scheduler-to-process swtch is counted once, including redispatch.
    switches = sum(pid in family for pid, _ in dispatches)
    return switches, statistics.mean(dispatched[p] - created[p] for p in family), \
        statistics.mean(exited[p] - created[p] for p in family)


def main():
    output = EVIDENCE / "results"
    output.mkdir(exist_ok=True)
    rows = []
    for policy in POLICIES:
        values = []
        for workload in WORKLOADS:
            paths = sorted((EVIDENCE / "runner_logs" / workload / policy).glob("run-*.log"))
            if not paths:
                raise SystemExit(f"Missing real logs for {workload}/{policy}; run ./runner.sh")
            samples = [metrics(path) for path in paths]
            means = [statistics.mean(s[i] for s in samples) for i in range(3)]
            deviations = [statistics.stdev(s[i] for s in samples) if len(samples) > 1 else 0 for i in range(3)]
            values.append((means, deviations))
            rows.append([policy, workload, len(samples), *means, *deviations])
        for i, (metric, label) in enumerate((('context_switches', 'Scheduler dispatches into workload family'),
                                             ('response', 'Mean response (ticks)'),
                                             ('turnaround', 'Mean turnaround (ticks)'))):
            fig, ax = plt.subplots(figsize=(11, 5), layout='constrained')
            ax.bar(WORKLOADS, [v[0][i] for v in values], yerr=[v[1][i] for v in values], capsize=3)
            ax.set(ylabel=label, title=f"{policy}: {label} (error bars: sample SD)")
            ax.set_ylim(bottom=0)
            ax.tick_params(axis='x', rotation=25)
            fig.savefig(output / f"phase6_{policy.lower()}_{metric}.png", dpi=160)
            plt.close(fig)
    with (output / "summary.csv").open('w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(['policy', 'workload', 'runs', 'switches', 'response', 'turnaround',
                         'switches_sd', 'response_sd', 'turnaround_sd'])
        writer.writerows(rows)
    print(f"PASS: generated nine plots and summary.csv in {output}")


if __name__ == '__main__':
    main()
