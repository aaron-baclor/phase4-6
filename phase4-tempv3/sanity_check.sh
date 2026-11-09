#!/usr/bin/env bash
# Section 4.3.2: one virtual CPU for every policy test.
set -euo pipefail
cd -- "$(dirname -- "$0")"
export CPUS=1
python3 sanity_check.py "$@"
