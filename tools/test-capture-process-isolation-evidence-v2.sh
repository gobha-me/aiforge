#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
capture="$repo_root/tools/capture-process-isolation-evidence-v2.sh"
fixture=$(mktemp -d)
cleanup_fixture() {
  find "$fixture" -depth -delete
}
trap cleanup_fixture EXIT
fake_bin="$fixture/bin"
mkdir "$fake_bin"

cat >"$fake_bin/sudo" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ ${1-} == -n ]]; then shift; fi
exec "$@"
EOF
cat >"$fake_bin/timeout" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
shift
exec "$@"
EOF
cat >"$fake_bin/sleep" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"$fake_bin/systemd-run" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf 'systemd-run %s\n' "$*" >>"$FAKE_LOG"
printf active >"$FAKE_STATE"
if [[ ${FAKE_SEND_TERM-0} == 1 ]]; then
  kill -TERM "$PPID"
fi
exit "${FAKE_CAPTURE_STATUS-0}"
EOF
cat >"$fake_bin/systemctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
action=$1
shift
printf 'systemctl %s %s\n' "$action" "$*" >>"$FAKE_LOG"
state=$(<"$FAKE_STATE")
case "$action" in
  show)
    if [[ $* == *--property=LoadState* ]]; then
      [[ $state == absent ]] && printf 'not-found\n' || printf 'loaded\n'
    elif [[ $* == *--property=ActiveState* ]]; then
      [[ $state == absent ]] && printf 'inactive\n' || printf 'active\n'
    else
      exit 2
    fi
    ;;
  stop)
    if [[ ${FAKE_CLEANUP_FAIL-0} == 1 ]]; then exit 1; fi
    printf absent >"$FAKE_STATE"
    ;;
  kill)
    if [[ ${FAKE_CLEANUP_FAIL-0} == 1 ]]; then exit 1; fi
    printf absent >"$FAKE_STATE"
    ;;
  reset-failed) ;;
  *) exit 2 ;;
esac
EOF
chmod u+x "$fake_bin"/*

run_capture() {
  local capture_status=$1 cleanup_fails=$2 sends_term=$3 initial_state=${4-absent}
  : >"$fixture/log"
  printf '%s' "$initial_state" >"$fixture/state"
  set +e
  PATH="$fake_bin:$PATH" \
    FAKE_LOG="$fixture/log" \
    FAKE_STATE="$fixture/state" \
    FAKE_CAPTURE_STATUS="$capture_status" \
    FAKE_CLEANUP_FAIL="$cleanup_fails" \
    FAKE_SEND_TERM="$sends_term" \
    bash "$capture" /bin/true aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
      "$fixture/report.json" >/dev/null 2>&1
  observed=$?
  set -e
}

run_capture 0 0 0
if ((observed != 0)) || [[ $(<"$fixture/state") != absent ]] ||
  ! grep -Fq -- '--expand-environment=no' "$fixture/log" ||
  ! grep -Fq -- '--property=Delegate=yes' "$fixture/log" ||
  ! grep -Fq -- '--property=DelegateSubgroup=aiforge-evaluator' \
    "$fixture/log" ||
  ! grep -Fq -- '${relative##*/}' "$fixture/log" ||
  ! grep -Fq -- '$root/cgroup.procs' "$fixture/log" ||
  ! grep -Fq -- '${processes[0]} == $$' "$fixture/log" ||
  ! grep -Eq '^systemctl stop aiforge-evidence-v2-[0-9]+\.service$' \
    "$fixture/log"; then
  echo "error: accepted capture did not remove its exact transient unit" >&2
  exit 1
fi
for preflight in \
  'systemd did not place the evaluator in its delegated subgroup' \
  'delegated unit root is absent' \
  'delegated unit root is not owned by the evaluator' \
  'delegated unit root controls are not writable' \
  'delegated unit root is not cgroup v2' \
  'delegated unit root is not a domain cgroup' \
  'delegated supervisor is not owned by the evaluator' \
  'delegated supervisor controls are not writable' \
  'delegated supervisor is not cgroup v2' \
  'delegated supervisor is not a domain cgroup' \
  'controller is unavailable' \
  'delegated unit root contains a process' \
  'delegated unit root controllers are already enabled' \
  'delegated supervisor controllers are already enabled' \
  'required controllers could not be enabled' \
  'required controllers were not enabled exactly' \
  'controller was not enabled' \
  'controller did not reach the supervisor' \
  'delegated supervisor does not exclusively contain the evaluator' \
  'delegated supervisor contains child cgroups' \
  'delegated unit root contains an unexpected cgroup'; do
  if ! grep -Fq "$preflight" "$fixture/log"; then
    echo "error: delegated cgroup preflight diagnostic is missing" >&2
    exit 1
  fi
done

run_capture 0 1 0
if ((observed == 0)) ||
  ! grep -Eq '^systemctl kill .* aiforge-evidence-v2-[0-9]+\.service$' \
    "$fixture/log"; then
  echo "error: transient-unit cleanup failure did not dominate acceptance" >&2
  exit 1
fi

run_capture 7 0 0
if ((observed != 7)) || [[ $(<"$fixture/state") != absent ]]; then
  echo "error: capture failure or cleanup postcondition was not preserved" >&2
  exit 1
fi

run_capture 0 0 1
if ((observed != 143)) || [[ $(<"$fixture/state") != absent ]]; then
  echo "error: TERM did not trigger exact transient-unit cleanup" >&2
  exit 1
fi

run_capture 0 0 0 active
if ((observed == 0)) || [[ $(<"$fixture/state") != active ]] ||
  grep -Eq '^(systemd-run|systemctl (stop|kill))' "$fixture/log"; then
  echo "error: a pre-existing transient-unit name was not preserved" >&2
  exit 1
fi

echo "process-isolation capture cleanup tests passed"
