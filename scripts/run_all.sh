#!/usr/bin/env bash
# Full pipeline: build everything, test everything, benchmark everything.
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/build_all.sh
./scripts/run_tests.sh
./scripts/run_benchmarks.sh
