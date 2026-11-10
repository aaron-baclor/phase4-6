"""Sections 4.3.2-4.3.4: shared one-CPU xv6 execution and log parsing."""
from pathlib import Path
import os
import re
import subprocess
import pexpect

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = Path(os.environ.get("CS140_EVIDENCE_DIR", str(ROOT))).resolve()
POLICIES = ("RR", "STCF", "GROUP_STCF")
N_GROUPS = int(re.search(r"#define N_GROUPS (\d+)", (ROOT / "kernel/stcf.h").read_text())[1])
WORKLOADS = ("test_cpu", "test_io", "test_f_cpu", "test_f_io", "test_bg",
             "test_group_iso", "test_group_sh")
HEADER = re.compile(r"^(\d+)\|(STCF|GROUP)\|(\d+)\((\d+),(\d+)\)(.*)$")
ENTRY = re.compile(r",\[(\d+)\]([^:,]*):([1-5])\((\d+),(\d+)\)")
# xv6 prints user strings a character at a time; kernel records can interrupt
# them. Match complete kernel records anywhere, retaining the original log.
RECORD = re.compile(r"(?:TRACE\|(?:CREATE|EXIT|DISPATCH|TICK|READY)\|[^\r\n]*|"
                    r"\d+\|(?:STCF|GROUP)\|[^\r\n]*)(?:\r?\n)")


def user_console(text):
    return RECORD.sub('', text)


def kernel_records(text):
    return [match.group(0).strip() for match in RECORD.finditer(text)]


def build(policy, trace=1):
    (EVIDENCE / "verification").mkdir(parents=True, exist_ok=True)
    path = EVIDENCE / "verification" / f"build-{policy}-trace{trace}.log"
    with path.open("w") as log:
        subprocess.run(["make", "-j4", "CPUS=1", f"SCHEDULING_POLICY={policy}",
                        f"TRACE={trace}"], cwd=ROOT, stdout=log,
                       stderr=subprocess.STDOUT, check=True)


class Session:
    def __init__(self, policy, path, trace=1, default=False):
        self.log = Path(path).open("w")
        temporary = EVIDENCE / 'verification/qemu-temp'
        temporary.mkdir(parents=True, exist_ok=True)
        args = ["qemu", "CPUS=1", f"TRACE={trace}", "QEMU=qemu-system-riscv64 -snapshot"]
        if not default:
            args.append(f"SCHEDULING_POLICY={policy}")
        self.child = pexpect.spawn("make", args, cwd=str(ROOT),
                                   encoding="utf-8", timeout=180,
                                   env={**os.environ, "CPUS": "1", "TMPDIR": str(temporary)}, echo=False)
        self.child.logfile_read = self.log
        try:
            self.child.expect_exact(f"Using Policy: {policy}")
            self.wait_prompt()
        except BaseException:
            self.close()
            raise

    def run(self, command, marker):
        self.child.sendline(command)
        self.wait_prompt()
        output = user_console(self.child.before)
        if output.startswith(command + '\r\n') or output.startswith(command + '\n'):
            output = output.split('\n', 1)[1]
        assert marker in output, f"Missing {marker!r} after command {command!r}: {output[-1000:]}"
        self.log.flush()

    def wait_prompt(self):
        self.child.expect(r"\$(?:" + RECORD.pattern + r")* ")

    def close(self):
        if self.child.isalive():
            self.child.send("\x01x")
            self.child.expect(pexpect.EOF)
        self.log.close()


def frames(text, grouped):
    result = []
    current = []
    for line in kernel_records(text):
        if not re.match(r"^\d+\|(?:STCF|GROUP)\|", line):
            continue
        match = HEADER.fullmatch(line)
        assert match, f"Malformed schedlog header: {line}"
        tick, policy, gid, shares, vruntime, tail = match.groups()
        entries = list(ENTRY.finditer(tail))
        assert "".join(m.group(0) for m in entries) == tail, f"Malformed entries: {line}"
        assert policy == ("GROUP" if grouped else "STCF")
        if not grouped:
            assert (gid, shares, vruntime) == ("0", "0", "0")
        row = (int(tick), int(gid), int(shares), int(vruntime),
               [(int(m[1]), m[2], int(m[3]), int(m[4]), int(m[5])) for m in entries])
        current.append(row)
        if not grouped or int(gid) == N_GROUPS - 1:
            if grouped:
                assert [r[1] for r in current] == list(range(N_GROUPS))
                assert len({r[0] for r in current}) == 1
                pids = [p[0] for r in current for p in r[4]]
                assert len(pids) == len(set(pids)), "Duplicate group membership"
            result.append(current)
            current = []
    assert not current, "Incomplete group snapshot"
    assert result, "No schedlog output"
    return result
