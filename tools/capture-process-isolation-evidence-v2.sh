#!/usr/bin/env bash
set -euo pipefail

if (($# != 3)); then
  echo "usage: $0 <evaluator> <source-sha> <output>" >&2
  exit 2
fi

evaluator=$1
source_sha=$2
output=$3
if [[ $evaluator != /* || ! -x $evaluator || $output != /* ||
      ! $source_sha =~ ^[0-9a-f]{40}$ ]]; then
  echo "error: invalid process-isolation evidence arguments" >&2
  exit 2
fi
for required in systemctl systemd-run sudo id sleep timeout; do
  if ! command -v "$required" >/dev/null 2>&1; then
    echo "error: delegated cgroup evidence requires $required" >&2
    exit 1
  fi
done

runner_user=$(id -un)
runner_group=$(id -gn)
unit="aiforge-evidence-v2-$$.service"
unit_may_exist=0

systemctl_bounded() {
  timeout 15s sudo -n systemctl "$@"
}

unit_is_absent() {
  local load_state active_state
  load_state=$(systemctl_bounded show "$unit" --property=LoadState --value \
    2>/dev/null) || return 1
  active_state=$(systemctl_bounded show "$unit" --property=ActiveState --value \
    2>/dev/null) || return 1
  [[ $load_state == not-found && $active_state == inactive ]]
}

cleanup_unit() {
  local status=$?
  trap - EXIT
  trap '' HUP INT TERM
  if ((unit_may_exist)); then
    if ! unit_is_absent; then
      systemctl_bounded stop "$unit" >/dev/null 2>&1 || true
      for _ in {1..10}; do
        if unit_is_absent; then
          exit "$status"
        fi
        sleep 0.1
      done
      systemctl_bounded kill --kill-who=all --signal=SIGKILL "$unit" \
        >/dev/null 2>&1 || true
      systemctl_bounded stop "$unit" >/dev/null 2>&1 || true
      systemctl_bounded reset-failed "$unit" >/dev/null 2>&1 || true
      for _ in {1..50}; do
        if unit_is_absent; then
          exit "$status"
        fi
        sleep 0.1
      done
      echo "error: transient delegated cgroup unit cleanup failed: $unit" >&2
      exit 1
    fi
  fi
  exit "$status"
}

trap cleanup_unit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if ! unit_is_absent; then
  echo "error: transient delegated cgroup unit name is not unused: $unit" >&2
  exit 1
fi

# The transient service contains only the shell and then, via exec, the
# evaluator itself. Delegate=yes makes the unit cgroup an explicit disposable
# delegation; the evaluator still validates ownership, emptiness, controllers,
# placement, and rollback before accepting any row as enforced.
unit_may_exist=1
timeout 300s sudo -n systemd-run --quiet --wait --pipe --collect \
  --unit="$unit" \
  --service-type=exec \
  --property=Delegate=yes \
  --uid="$runner_user" \
  --gid="$runner_group" \
  /bin/bash -c '
    set -euo pipefail
    relative=
    while IFS= read -r entry; do
      if [[ $entry == 0::* ]]; then
        if [[ -n $relative ]]; then
          echo "error: transient delegated cgroup identity is ambiguous" >&2
          exit 1
        fi
        relative=${entry#0::}
      fi
    done < /proc/self/cgroup
    if [[ $relative != /* || $relative == / ]]; then
      echo "error: transient delegated cgroup identity is invalid" >&2
      exit 1
    fi
    exec "$1" --source-sha "$2" --output "$3" \
      --delegated-cgroup-root "/sys/fs/cgroup${relative}"
  ' _ "$evaluator" "$source_sha" "$output"
