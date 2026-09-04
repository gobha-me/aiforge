#!/usr/bin/env bash
set -euo pipefail

if (($# != 0)); then
  echo "usage: tools/lint.sh" >&2
  exit 2
fi

tidy=${CLANG_TIDY:-clang-tidy-20}
runner=${RUN_CLANG_TIDY:-run-clang-tidy-20}
compiler=${CLANGXX:-clang++-20}
build_dir=${AIFORGE_TIDY_BUILD_DIR:-build-tidy}
jobs=${AIFORGE_TIDY_JOBS:-2}
baseline=tools/clang-tidy-baseline.tsv

for tool in "$tidy" "$runner" "$compiler" python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: $tool was not found; install Clang/clang-tidy 20.x" >&2
    exit 1
  fi
done

tidy_version=$($tidy --version)
if [[ ! "$tidy_version" =~ version[[:space:]]20\. ]]; then
  echo "error: AIForge linting requires clang-tidy 20.x; found: $tidy_version" >&2
  exit 1
fi

compiler_version=$($compiler --version)
if [[ ! "$compiler_version" =~ version[[:space:]]20\. ]]; then
  echo "error: AIForge linting requires Clang 20.x; found: $compiler_version" >&2
  exit 1
fi

if [[ ! "$jobs" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: AIFORGE_TIDY_JOBS must be a positive integer" >&2
  exit 2
fi

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

tools/check-nolint.sh
"$tidy" --verify-config

cmake -S . -B "$build_dir" \
  -DCMAKE_CXX_COMPILER="$compiler" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -Daiforge_TESTS=OFF \
  -Daiforge_DRAWFORGE_EVALUATION=OFF \
  -Daiforge_PROCESS_ISOLATION_EVALUATION=ON

output=$(mktemp)
trap 'rm -f "$output"' EXIT

set +e
"$runner" \
  -p "$build_dir" \
  -j "$jobs" \
  -clang-tidy-binary "$tidy" \
  -config-file "$repo_root/.clang-tidy" \
  -header-filter='^.*/(include/aiforge|src/(lib|adapters|bin))/' \
  -exclude-header-filter='^.*/(build[^/]*/|_deps/)/' \
  -quiet \
  "^${repo_root}/src/(lib|adapters|bin)/.*[.]cpp$" >"$output" 2>&1
status=$?
set -e

if ((status != 0)); then
  cat "$output" >&2
  echo "error: clang-tidy execution failed" >&2
  exit "$status"
fi

tools/check-clang-tidy.py "$output" "$baseline"

"$runner" \
  -p "$build_dir" \
  -j "$jobs" \
  -clang-tidy-binary "$tidy" \
  -config-file "$repo_root/evaluation/process_isolation/.clang-tidy" \
  -header-filter='^.*/evaluation/process_isolation/' \
  -exclude-header-filter='^.*/(build[^/]*/|_deps/)/' \
  -warnings-as-errors='*' \
  -quiet \
  "^${repo_root}/evaluation/process_isolation/(child_main|evidence|main|probes|runner)[.]cpp$"
