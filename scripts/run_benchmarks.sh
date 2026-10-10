#!/usr/bin/env bash
# Run every benchmark, in sequence, from the Release build only.
#
# Sequential on purpose: these measure wall-clock time and CPU time on real
# loopback sockets, so running two at once would make both numbers meaningless.
# Nothing here is retried or filtered -- whatever the run produces is what
# lands in results/.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -x build/release/bench_reliability ] || {
  echo "Release build missing -- run scripts/build_all.sh first" >&2; exit 1; }

mkdir -p results
{
  for b in reliability throughput window concurrency; do
    echo "########## bench_$b ##########"
    "./build/release/bench_$b" "$@"
    echo "### bench_$b exit=$?"
  done
} 2>&1 | tee results/bench_run.log

echo
echo "CSV written to results/*.csv, full log in results/bench_run.log"
