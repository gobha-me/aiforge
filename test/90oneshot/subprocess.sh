#!/usr/bin/env bash
set -euo pipefail

fixture=$1
production=${2:-}
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT

printf 'stdin evidence' | "$fixture" explain >"$test_dir/out" 2>"$test_dir/err"
[[ $(cat "$test_dir/out") == answer ]]
grep -q '^citation: https://example.test (fixture)$' "$test_dir/err"
grep -q '^usage: input=2 output=1 cached=0 reasoning=0$' "$test_dir/err"
grep -q '^cost: unavailable$' "$test_dir/err"
! grep -q 'citation:' "$test_dir/out"

set +e
printf 'stdin evidence' | "$fixture" >"$test_dir/out" 2>"$test_dir/err"
status=$?
set -e
[[ $status -eq 2 ]]
[[ ! -s "$test_dir/out" ]]
grep -q 'prompt is required' "$test_dir/err"

set +e
"$fixture" cancel >"$test_dir/out" 2>"$test_dir/err" &
pid=$!
for _ in $(seq 1 100); do
  [[ -s "$test_dir/out" ]] && break
  sleep 0.01
done
kill -INT "$pid"
wait "$pid"
status=$?
set -e
[[ $status -eq 130 ]]
[[ $(cat "$test_dir/out") == partial ]]
grep -q 'request cancelled' "$test_dir/err"

set +e
set -o pipefail
"$fixture" flood 2>"$test_dir/err" | head -c 1 >"$test_dir/out"
status=$?
set +o pipefail
set -e
[[ $status -eq 1 ]]
[[ $(wc -c <"$test_dir/out") -eq 1 ]]
grep -q 'completion output failed' "$test_dir/err"

if [[ -n $production ]]; then
  mkdir "$test_dir/config"
  set +e
  printf 'stdin evidence' | env -u VENICE_API_KEY \
    XDG_CONFIG_HOME="$test_dir/config" AIFORGE_MODEL=fixture-model \
    "$production" explain >"$test_dir/out" 2>"$test_dir/err"
  status=$?
  set -e
  [[ $status -eq 1 ]]
  [[ ! -s "$test_dir/out" ]]
  grep -q 'Venice credential is not configured' "$test_dir/err"

  set +e
  printf 'must-not-be-read\n' | env -u VENICE_API_KEY \
    XDG_CONFIG_HOME="$test_dir/config" "$production" login \
    >"$test_dir/out" 2>"$test_dir/err"
  status=$?
  set -e
  [[ $status -eq 2 ]]
  [[ ! -e "$test_dir/config/aiforge/credentials" ]]
  grep -q 'credentials cannot be piped' "$test_dir/err"
  ! grep -q 'must-not-be-read' "$test_dir/out" "$test_dir/err"

  set +e
  printf 'stdin evidence' | VENICE_API_KEY='invalid value' \
    XDG_CONFIG_HOME="$test_dir/config" AIFORGE_MODEL=fixture-model \
    "$production" explain >"$test_dir/out" 2>"$test_dir/err"
  status=$?
  set -e
  [[ $status -eq 1 ]]
  grep -q 'VENICE_API_KEY is invalid' "$test_dir/err"
  ! grep -q 'invalid value' "$test_dir/out" "$test_dir/err"
fi
