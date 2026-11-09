#!/usr/bin/env bash
# Section 4.3.2: always run from this Phase 6 copy with one virtual CPU.
set -euo pipefail
cd -- "$(dirname -- "$0")"
export CPUS=1
python3 sanity_check.py "$@"
