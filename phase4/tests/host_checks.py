#!/usr/bin/env python3
"""Extract actual Phase 4 scheduler functions for focused unit checks."""
from pathlib import Path
import os
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = ('stcf_erbt', 'find_shortest_process', 'find_lowest_gid', 'stcf_should_preempt',
             'scheduler_tick', 'wakeup', 'kill', 'proc_setburst', 'proc_schedlog', 'schedlog_emit')
def main():
    source = (ROOT / 'kernel/proc.c').read_text(); extracted = []
    for name in FUNCTIONS:
        match = re.search(r'(?m)^(?:static )?(?:struct proc\*|void|int)\n' + name + r'\(', source)
        assert match, name
        start = source.index('{', match.end()); end = start + 1; depth = 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}'); end += 1
        extracted.append(source[match.start():end])
    output = Path(os.environ.get('CS140_EVIDENCE_DIR', str(ROOT))) / 'verification/host'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'paired_functions.inc').write_text('\n\n'.join(extracted))
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-I.', '-I' + str(output), 'tests/host_checks.c', '-o', str(output / 'checks')], cwd=ROOT, check=True)
    subprocess.run([str(output / 'checks')], cwd=ROOT, check=True)

if __name__ == '__main__': main()
