#!/usr/bin/env python3
"""Print concise, genuine transcript excerpts for the screenshot checklist."""
import argparse
import re
from qemu_tools import ROOT, EVIDENCE, frames, user_console, kernel_records


def lines(path):
    if not path.exists():
        raise SystemExit(f"Evidence not generated: {path}")
    return path.read_text().splitlines()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('topic', choices=('membership', 'validation', 'group_log', 'preemption',
                                          'aging', 'shares', 'isolation', 'rr', 'host', 'results'))
    topic = parser.parse_args().topic
    transcript = EVIDENCE / 'verification/sanity-GROUP_STCF.log'
    raw = transcript.read_text()
    data = user_console(raw).splitlines()
    records = kernel_records(raw)
    if topic == 'membership':
        for line in data:
            match = re.fullmatch(r'MEMBER\|(\d+)\|(\d+)\|(\w+)', line)
            if match:
                print(line)
                if match[3] in ('inherit', 'groupfork'):
                    print(next(item for item in records if item.startswith(f'TRACE|DISPATCH|{match[1]}|')))
            elif 'exec preserved membership' in line:
                print(line)
    elif topic == 'validation':
        for policy in ('RR', 'STCF', 'GROUP_STCF'):
            print(f'--- {policy}: verification/sanity-{policy}.log ---')
            for line in user_console((EVIDENCE / f'verification/sanity-{policy}.log').read_text()).splitlines():
                if line.startswith(('PASS: fatal', 'PASS: shares', 'EXHAUST|', 'PASS: test_sched')):
                    print(line)
    elif topic == 'group_log':
        parsed = frames(raw, True)
        frame = next(frame for frame in parsed if any(row[1] == 7 and row[4] for row in frame))
        tick = frame[0][0]
        start = next(i for i, line in enumerate(records) if line.startswith(f'{tick}|GROUP|0(')
                     and any(item.startswith(f'{tick}|GROUP|7(') and ',[' in item for item in records[i:i+8]))
        print('\n'.join(records[start:start+8]))
    elif topic in ('preemption', 'aging'):
        for policy in ('STCF', 'GROUP_STCF'):
            print(f'--- {policy} ---')
            raw_source = (EVIDENCE / f'verification/sanity-{policy}.log').read_text()
            source = user_console(raw_source).splitlines()
            marker = 'SHORT_READY' if topic == 'preemption' else 'AGING_RELEASE'
            event = next(line for line in source if line.startswith(marker + '|'))
            pid, tick = map(int, event.split('|')[1:3])
            print(event)
            for line in kernel_records(raw_source) + source:
                if line.startswith(f'TRACE|READY|{pid}|') and topic == 'aging':
                    if int(line.split('|')[4]) in (19, 20, 40, 59, 60):
                        print(line)
                elif line.startswith((f'TRACE|DISPATCH|{pid}|', f'TRACE|TICK|{pid}|', f'TRACE|EXIT|{pid}|')):
                    if int(line.split('|')[3]) >= tick:
                        print(line)
                elif line.startswith(('AGED_RUN|', 'PASS: wakeup' if topic == 'preemption' else 'PASS: starvation')):
                    if topic == 'aging' or line.startswith('PASS: wakeup'):
                        print(line)
    elif topic == 'shares':
        for line in lines(EVIDENCE / 'workload-checks.log'):
            if 'shares ' in line or line.startswith('PASS: 140'):
                print(line)
    elif topic == 'isolation':
        path = EVIDENCE / 'runner_logs/test_group_iso/GROUP_STCF/run-01.log'
        raw_source = path.read_text()
        source = kernel_records(raw_source) + user_console(raw_source).splitlines()
        print(path.relative_to(EVIDENCE))
        print(next(line for line in source if re.fullmatch(r'TRACE\|TICK\|\d+\|\d+\|2', line)))
        for line in source:
            if line.startswith(('ISOLATION|', 'DONE|')):
                print(line)
        for line in lines(EVIDENCE / 'workload-checks.log'):
            if 'isolation run-01' in line:
                print(line)
    elif topic == 'rr':
        for line in user_console((EVIDENCE / 'verification/sanity-RR.log').read_text()).splitlines():
            if line.startswith(('Using Policy:', 'EXHAUST|', 'PASS:', 'fork test OK')) or 'existing-syscalls-ok' in line:
                print(line)
    elif topic == 'host':
        print('\n'.join([line for line in lines(EVIDENCE / 'sanity-summary.log')
                         if line.startswith('PASS:')][:5]))
    elif topic == 'results':
        print('\n'.join(lines(EVIDENCE / 'results-summary.log')))
        print('\n'.join(lines(EVIDENCE / 'results/summary.csv')))


if __name__ == '__main__':
    main()
