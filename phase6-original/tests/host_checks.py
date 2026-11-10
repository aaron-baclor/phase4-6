#!/usr/bin/env python3
"""Section 4.3.2: compile actual proc.c functions against checked lock stubs."""
from pathlib import Path
import re
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = (
    "proc_setstate", "stcf_erbt", "find_shortest_process", "find_lowest_gid",
    "stcf_should_preempt", "scheduler_tick", "proc_setburst",
    "proc_setgroup", "proc_setshares", "proc_schedlog", "schedlog_emit", "kill",
)


def main():
    source = (ROOT / "kernel/proc.c").read_text()
    extracted = []
    for name in FUNCTIONS:
        match = re.search(r"(?m)^(?:static )?(?:struct proc\*|void|int)\n" + name + r"\(", source)
        if not match:
            raise AssertionError(f"Function missing: {name}")
        start = source.index("{", match.end())
        depth = 1
        end = start + 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        extracted.append(source[match.start():end])
    output = Path(os.environ.get("CS140_EVIDENCE_DIR", str(ROOT))) / "verification/host"
    output.mkdir(parents=True, exist_ok=True)
    (output / "tested_functions.inc").write_text("\n\n".join(extracted))
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I.",
                    "-I" + str(output), "tests/host_checks.c", "-o", str(output / "host_checks")],
                   cwd=ROOT, check=True)
    subprocess.run([str(output / "host_checks")], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
