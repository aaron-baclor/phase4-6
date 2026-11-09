#!/usr/bin/env bash
# Section 4.3.3: default 10 repetitions; N=20 overrides as in Section 4.3.
set -euo pipefail
cd -- "$(dirname -- "$0")"
export CPUS=1
python3 tests/run_workloads.py --runs "${N:-10}" --jobs "${JOBS:-1}" "$@"
