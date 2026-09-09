#!/usr/bin/env bash
set -euo pipefail
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK_DIR=$(mktemp -d)
trap 'rm -rf "${WORK_DIR}"' EXIT
COMMON_CMAKE_ARGS=("$@")
if [[ $(uname -s) != Linux ]]; then
  exit 0
fi
for kind in installed preexisting; do
  cmake -S "${SOURCE_DIR}/cmake/dependency-probe" -B "${WORK_DIR}/${kind}" \
    "${COMMON_CMAKE_ARGS[@]}" -DAIFORGE_SOURCE_DIR="${SOURCE_DIR}" \
    -DPROBE_DEPENDENCY=systemd_journal -DPROBE_SYSTEMD_PREEXISTING="$([[ ${kind} == preexisting ]] && echo ON || echo OFF)"
  cmake --build "${WORK_DIR}/${kind}" --parallel 2
  "${WORK_DIR}/${kind}/dependency_probe"
done

mkdir -p "${WORK_DIR}/headers/systemd"
printf '#error Deliberately invalid task-owned journal header\n' > "${WORK_DIR}/headers/systemd/sd-journal.h"
for kind in absent obsolete header symbols; do
  package_root="${WORK_DIR}/${kind}-package"
  mkdir -p "${package_root}"
  if [[ ${kind} != absent ]]; then
    package_version=255
    package_cflags=
    package_libs=-lsystemd
    if [[ ${kind} == obsolete ]]; then package_version=232; fi
    if [[ ${kind} == header ]]; then package_cflags="-I${WORK_DIR}/headers"; fi
    if [[ ${kind} == symbols ]]; then package_libs=-lm; fi
    cat > "${package_root}/libsystemd.pc" <<PC
Name: task-owned libsystemd fixture
Description: deliberately incompatible dependency fixture
Version: ${package_version}
Libs: ${package_libs}
Cflags: ${package_cflags}
PC
  fi
  if env PKG_CONFIG_LIBDIR="${package_root}" PKG_CONFIG_PATH= \
    cmake -S "${SOURCE_DIR}/cmake/dependency-probe" -B "${WORK_DIR}/${kind}" \
      "${COMMON_CMAKE_ARGS[@]}" -DAIFORGE_SOURCE_DIR="${SOURCE_DIR}" \
      -DPROBE_DEPENDENCY=systemd_journal -DPROBE_SYSTEMD_PREEXISTING=OFF \
      -DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=FALSE > "${WORK_DIR}/${kind}.log" 2>&1; then
    echo "Invalid system journal dependency unexpectedly configured: ${kind}" >&2
    exit 1
  fi
  if [[ ${kind} == header || ${kind} == symbols ]]; then
    # A successful capability cache must not survive changed target inputs.
    cmake -S "${SOURCE_DIR}/cmake/dependency-probe" -B "${WORK_DIR}/${kind}-reconfigure" \
      "${COMMON_CMAKE_ARGS[@]}" -DAIFORGE_SOURCE_DIR="${SOURCE_DIR}" \
      -DPROBE_DEPENDENCY=systemd_journal -DPROBE_SYSTEMD_PREEXISTING=OFF
    if env PKG_CONFIG_LIBDIR="${package_root}" PKG_CONFIG_PATH= \
      cmake -S "${SOURCE_DIR}/cmake/dependency-probe" -B "${WORK_DIR}/${kind}-reconfigure" \
        "${COMMON_CMAKE_ARGS[@]}" -DAIFORGE_SOURCE_DIR="${SOURCE_DIR}" \
        -DPROBE_DEPENDENCY=systemd_journal -DPROBE_SYSTEMD_PREEXISTING=OFF \
        -DPKG_CONFIG_USE_CMAKE_PREFIX_PATH=FALSE \
        -U '*AIFORGE_SYSTEMD_JOURNAL*' > "${WORK_DIR}/${kind}-reconfigure.log" 2>&1; then
      echo "Changed system journal dependency retained a stale capability result: ${kind}" >&2
      exit 1
    fi
    if ! grep -Eq 'SystemdJournal target lacks required client headers or symbols' "${WORK_DIR}/${kind}-reconfigure.log"; then
      cat "${WORK_DIR}/${kind}-reconfigure.log" >&2
      exit 1
    fi
  fi
  if ! grep -Eq 'SystemdJournal' "${WORK_DIR}/${kind}.log"; then
    cat "${WORK_DIR}/${kind}.log" >&2
    exit 1
  fi
done
