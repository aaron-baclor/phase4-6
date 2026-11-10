#!/usr/bin/env python3
"""Sections 2.5, 3.6, 4.3: assert isolation, live shares and runtime history."""
import re
from qemu_tools import ROOT, EVIDENCE, WORKLOADS, frames, user_console, kernel_records


def main():
    count = 0
    for workload in WORKLOADS:
        paths = sorted((EVIDENCE / "runner_logs" / workload / "GROUP_STCF").glob("run-*.log"))
        if not paths:
            raise AssertionError(f"Missing workload evidence: {workload}")
        for path in paths:
            text = path.read_text()
            console = user_console(text)
            snapshots = frames(text, True)
            previous = {}
            for frame in snapshots:
                for tick, gid, shares, runtime, entries in frame:
                    assert runtime >= previous.get(gid, 0), (path, gid, tick)
                    previous[gid] = runtime
            events = [(int(pid), int(tick), int(gid)) for pid, tick, gid in
                      re.findall(r"TRACE\|TICK\|(\d+)\|(\d+)\|(\d+)", text)]
            if workload == "test_group_sh":
                change = re.search(r"SHARES_CHANGE\|(\d+)", console)
                assert change, path
                change = int(change[1])
                before = [sum(gid == g and tick < change for _, tick, gid in events) for g in (1, 2)]
                after = [sum(gid == g and tick > change for _, tick, gid in events) for g in (1, 2)]
                assert min(before + after) > 0, (path, before, after)
                assert before[1] >= 2 * before[0], (path, before)
                assert after[0] >= 2 * after[1], (path, after)
                print(f"PASS: shares {path.name}: before={before}, after={after}; virtual runtime never decreased")
            if workload == "test_group_iso":
                assert "ISOLATION|1|complete" in console and "ISOLATION|2|complete" in console, path
                records = kernel_records(text)
                flood = next(re.fullmatch(r"TRACE\|DISPATCH\|(\d+)\|\d+\|1", item)[1]
                             for item in records if re.fullmatch(r"TRACE\|DISPATCH\|(\d+)\|\d+\|1", item))
                children = {re.fullmatch(r"TRACE\|CREATE\|(\d+)\|\d+\|" + flood, item)[1]
                            for item in records if re.fullmatch(r"TRACE\|CREATE\|(\d+)\|\d+\|" + flood, item)}
                children = {pid for pid in children if int(pid) % 4 == 1}
                assert children, path
                last = max(i for i, item in enumerate(records) if any(item.startswith(f"TRACE|EXIT|{pid}|") for pid in children))
                long_worker = re.search(r"ISOLATION_WORKER\|2\|(\d+)", console)
                assert long_worker, path
                assert any(re.fullmatch(r"TRACE\|TICK\|" + long_worker[1] + r"\|\d+\|2", item) for item in records[:last]), path
                assert any(gid == 1 for _, _, gid in events), path
                print(f"PASS: isolation {path.name}: Group 2 consumed CPU before Group 1 flood completed")
            count += 1
    print(f"PASS: {count} grouped workload logs, unique membership, exact schema and monotonic history")


if __name__ == "__main__":
    main()
