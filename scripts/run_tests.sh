#!/usr/bin/env bash
# Run the correctness suite under every configuration that was built.
#
# Exit status is non-zero if any configuration reports a failure, so this is
# safe to use as a CI gate.
set -uo pipefail
cd "$(dirname "$0")/.."

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:detect_stack_use_after_return=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}"
export TSAN_OPTIONS="${TSAN_OPTIONS:-second_deadlock_stack=1}"

mkdir -p results
rc=0
for cfg in release debug asan tsan; do
  bin="build/$cfg/rudp_tests"
  [ -x "$bin" ] || { echo "skipping $cfg (not built)"; continue; }
  echo "=================== $cfg ==================="
  "$bin" "$@" 2>&1 | tee "results/tests_$cfg.log"
  status="${PIPESTATUS[0]}"
  [ "$status" -eq 0 ] || { echo "*** $cfg FAILED (exit $status) ***"; rc=1; }
done
exit "$rc"
