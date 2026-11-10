#!/usr/bin/env python3
"""Sections 2.3-2.5/4.3.2: Phase 4 host checks and one-CPU xv6 probes."""
import re
import subprocess
import sys
from tests.qemu_tools import ROOT, EVIDENCE, POLICIES, Session, build, frames, user_console

def main():
    output = EVIDENCE / 'verification'; output.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, '-m', 'unittest', 'tests.test_log_parser'], cwd=ROOT, check=True)
    subprocess.run([sys.executable, 'tests/host_checks.py'], cwd=ROOT, check=True)
    for policy in POLICIES:
        build(policy)
        path = output / f'sanity-{policy}.log'; session = Session(policy, path)
        try:
            session.run('test_phase4', 'PASS: test_phase4')
            session.run('forktest', 'fork test OK')
            session.run('echo existing-syscalls-ok', 'existing-syscalls-ok')
            if policy != 'RR':
                session.run('test_phase4 preempt', 'PASS: wakeup preemption')
                session.run('test_phase4 aging', 'PASS: starvation mitigation')
        finally:
            session.close()
        text = path.read_text(); console = user_console(text)
        assert 'FAIL:' not in console and 'panic:' not in text
        assert f"EXHAUST|{17 if policy == 'RR' else -1}" in console
        snapshots = frames(text, policy == 'GROUP_STCF')
        if policy != 'RR':
            release = re.search(r'AGING_RELEASE\|(\d+)\|(\d+)', console)
            aged = re.search(r'AGED_RUN\|(\d+)\|(\d+)', console)
            assert release and aged and release[1] == aged[1]
            assert 40 <= int(aged[2]) - int(release[2]) <= 65
        if policy == 'GROUP_STCF':
            first = {}
            for pid, gid in re.findall(r'TRACE\|DISPATCH\|(\d+)\|\d+\|(\d+)', text):
                first.setdefault(int(pid), int(gid))
            assert all(gid == (0 if pid <= 2 else pid % 4) for pid, gid in first.items())
        print(f'PASS: Phase 4 {policy}: ABI, fork/exec, budgets, preemption/aging and {len(snapshots)} log snapshots', flush=True)
    print('PASS: all Phase 4 sanity checks', flush=True)

if __name__ == '__main__': main()
