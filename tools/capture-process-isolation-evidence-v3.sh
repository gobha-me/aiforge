#!/usr/bin/env bash
set -euo pipefail

# Schema v3 needs the same exclusive, bounded delegated-cgroup lifecycle as
# schema v2. Keep the released v2 wrapper byte-for-byte immutable and delegate
# to it instead of maintaining a second security-sensitive implementation.
script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec bash "$script_directory/capture-process-isolation-evidence-v2.sh" "$@"
