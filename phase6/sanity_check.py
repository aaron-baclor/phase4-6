#!/usr/bin/env python3
"""Section 4.3.2: actual host logic checks plus xv6 ABI/integration checks."""
from pathlib import Path
import re
import subprocess
import sys
from tests.qemu_tools import ROOT, EVIDENCE, POLICIES, Session, build, frames, user_console


def main():
    output = EVIDENCE / "verification"
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, '-m', 'unittest', 'tests.test_log_parser'], cwd=ROOT, check=True)
    subprocess.run([sys.executable, "tests/host_checks.py"], cwd=ROOT, check=True)
    for policy in POLICIES:
        build(policy)
        path = output / f"sanity-{policy}.log"
        session = Session(policy, path)
        try:
            session.run("test_sched", "PASS: test_sched")
            session.run("echo existing-syscalls-ok", "existing-syscalls-ok")
            session.run("forktest", "fork test OK")
            if policy != "RR":
                session.run("test_sched preempt", "PASS: wakeup preemption")
                session.run("test_sched aging", "PASS: starvation mitigation")
        finally:
            session.close()
        text = path.read_text()
        console = user_console(text)
        assert "FAIL:" not in console and "panic:" not in text
        assert f"EXHAUST|{17 if policy == 'RR' else -1}" in console
        if policy != "RR":
            release = re.search(r"AGING_RELEASE\|(\d+)\|(\d+)", console)
            aged = re.search(r"AGED_RUN\|(\d+)\|(\d+)", console)
            assert release and aged and release[1] == aged[1]
            assert 40 <= int(aged[2]) - int(release[2]) <= 65
            ready = re.search(r"SHORT_READY\|(\d+)\|(\d+)", console)
            assert ready
            charges = re.findall(rf"TRACE\|TICK\|{ready[1]}\|(\d+)\|", text)
            assert charges and min(map(int, charges)) <= int(ready[2]) + 1
        snapshots = frames(text, policy == "GROUP_STCF")
        if policy == "GROUP_STCF":
            for pid, gid, action in re.findall(r"MEMBER\|(\d+)\|(\d+)\|(\w+)", console):
                appearances = [row[1] for frame in snapshots for row in frame
                               for entry in row[4] if entry[0] == int(pid)]
                assert int(gid) in appearances, (pid, gid, action)
            # Validate direct placement from the first dispatch, not a later move.
            for pid, gid, action in re.findall(r"MEMBER\|(\d+)\|(\d+)\|(inherit|groupfork)", console):
                first = re.search(rf"TRACE\|DISPATCH\|{pid}\|\d+\|(\d+)", text)
                assert first and first[1] == gid, (pid, action)
        print(f"PASS: {policy} syscall ABI, fatal validation, fork/exec, budgets, schedlog ({len(snapshots)} snapshots)", flush=True)
    path = output / "sanity-boost.log"
    session = Session("GROUP_STCF", path)
    try:
        session.run("test_group_sh", "DONE|test_group_sh")
    finally:
        session.close()
    assert any(frame[2][2] == 200 for frame in frames(path.read_text(), True))
    print("PASS: live automatic boost in a fresh guest", flush=True)
    print("PASS: all Phase 6 sanity checks", flush=True)


if __name__ == "__main__":
    main()
