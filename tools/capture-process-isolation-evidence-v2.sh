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
for required in systemctl systemd-run sudo id sleep stat timeout; do
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
# evaluator itself. DelegateSubgroup keeps that supervisor process out of the
# delegated unit root. The shell enables the required controllers at that empty
# root, making them available to the subgroup. The evaluator then performs its
# existing pinned bootstrap inside the subgroup before accepting any rows.
unit_may_exist=1
# Preserve the embedded Bash program verbatim. systemd-run otherwise expands
# its ${...} expressions in the manager before Bash can evaluate them.
timeout 300s sudo -n systemd-run --quiet --wait --pipe --collect \
  --expand-environment=no \
  --unit="$unit" \
  --service-type=exec \
  --property=Delegate=yes \
  --property=DelegateSubgroup=aiforge-evaluator \
  --uid="$runner_user" \
  --gid="$runner_group" \
  /bin/bash -c '
    set -euo pipefail
    fail_preflight() {
      echo "error: delegated cgroup preflight failed: $1" >&2
      exit 1
    }
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
    if [[ ${relative##*/} != aiforge-evaluator ]]; then
      fail_preflight "systemd did not place the evaluator in its delegated subgroup"
    fi
    supervisor="/sys/fs/cgroup${relative}"
    root=${supervisor%/*}
    [[ $root != /sys/fs/cgroup && -d $root ]] ||
      fail_preflight "delegated unit root is absent"
    [[ -O $root ]] || fail_preflight "delegated unit root is not owned by the evaluator"
    [[ -w $root && -w $root/cgroup.procs &&
       -w $root/cgroup.subtree_control ]] ||
      fail_preflight "delegated unit root controls are not writable"
    [[ $(stat -f -c %T -- "$root") == cgroup2fs ]] ||
      fail_preflight "delegated unit root is not cgroup v2"
    type=$(<"$root/cgroup.type")
    [[ $type == domain ]] || fail_preflight "delegated unit root is not a domain cgroup"
    [[ -d $supervisor && -O $supervisor ]] ||
      fail_preflight "delegated supervisor is not owned by the evaluator"
    [[ -w $supervisor && -w $supervisor/cgroup.procs &&
       -w $supervisor/cgroup.subtree_control ]] ||
      fail_preflight "delegated supervisor controls are not writable"
    [[ $(stat -f -c %T -- "$supervisor") == cgroup2fs ]] ||
      fail_preflight "delegated supervisor is not cgroup v2"
    supervisor_type=$(<"$supervisor/cgroup.type")
    [[ $supervisor_type == domain ]] ||
      fail_preflight "delegated supervisor is not a domain cgroup"
    controllers=$(<"$root/cgroup.controllers")
    for controller in cpu memory pids; do
      [[ " $controllers " == *" $controller "* ]] ||
        fail_preflight "required $controller controller is unavailable"
    done
    mapfile -t root_processes <"$root/cgroup.procs"
    [[ ${#root_processes[@]} -eq 0 ]] ||
      fail_preflight "delegated unit root contains a process"
    enabled=$(<"$root/cgroup.subtree_control")
    [[ -z $enabled ]] || fail_preflight "delegated unit root controllers are already enabled"
    mapfile -t processes <"$supervisor/cgroup.procs"
    [[ ${#processes[@]} -eq 1 && ${processes[0]} == $$ ]] ||
      fail_preflight "delegated supervisor does not exclusively contain the evaluator"
    supervisor_enabled=$(<"$supervisor/cgroup.subtree_control")
    [[ -z $supervisor_enabled ]] ||
      fail_preflight "delegated supervisor controllers are already enabled"
    shopt -s dotglob nullglob
    supervisor_children=("$supervisor"/*/)
    [[ ${#supervisor_children[@]} -eq 0 ]] ||
      fail_preflight "delegated supervisor contains child cgroups"
    root_children=("$root"/*/)
    [[ ${#root_children[@]} -eq 1 &&
       ${root_children[0]%/} == "$supervisor" ]] ||
      fail_preflight "delegated unit root contains an unexpected cgroup"
    if ! printf "%s" "+cpu +memory +pids" >"$root/cgroup.subtree_control"; then
      fail_preflight "required controllers could not be enabled"
    fi
    enabled=$(<"$root/cgroup.subtree_control")
    read -r -a enabled_controllers <<<"$enabled"
    [[ ${#enabled_controllers[@]} -eq 3 ]] ||
      fail_preflight "required controllers were not enabled exactly"
    for controller in cpu memory pids; do
      [[ " $enabled " == *" $controller "* ]] ||
        fail_preflight "required $controller controller was not enabled"
    done
    supervisor_controllers=$(<"$supervisor/cgroup.controllers")
    for controller in cpu memory pids; do
      [[ " $supervisor_controllers " == *" $controller "* ]] ||
        fail_preflight "required $controller controller did not reach the supervisor"
    done
    exec "$1" --source-sha "$2" --output "$3" \
      --delegated-cgroup-root "$supervisor"
  ' _ "$evaluator" "$source_sha" "$output"
