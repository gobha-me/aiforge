#!/usr/bin/env bash
# Runs only task-owned configure/link fixtures against an already installed
# Venice prefix. No provider requests, client connections or resolver runtime.
set -euo pipefail
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PREFIX=${1:?usage: verify-venice-transport-consumption.sh installed-prefix [cmake-options...]}
shift
COMMON_CMAKE_ARGS=("$@")
WORK_DIR=$(mktemp -d)
trap 'cmake -E remove_directory "${WORK_DIR}"' EXIT
configure() {
  local name=$1
  shift
  cmake -S "${SOURCE_DIR}/cmake/dependency-probe" -B "${WORK_DIR}/${name}" \
    "${COMMON_CMAKE_ARGS[@]}" -DAIFORGE_SOURCE_DIR="${SOURCE_DIR}" \
    -DPROBE_DEPENDENCY=venice_cpp -DPROBE_EXPECT_IMPORTED=ON \
    -DPROBE_VENICE_PREEXISTING=ON -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_VENICE-CPP="${SOURCE_DIR}/cmake/dependency-probe/mismatched" \
    "$@"
}
mapfile -t exports < <(rg --files "${PREFIX}" | rg '/venice-cppTargets\.cmake$')
if [[ ${#exports[@]} != 1 ]]; then
  echo "Expected one actual Venice target export in task-owned prefix" >&2
  exit 1
fi
configure direct -DPROBE_VENICE_DIRECT_TARGET=ON -DPROBE_VENICE_EXPORTS="${exports[0]}"
cmake --build "${WORK_DIR}/direct" --parallel 2
"${WORK_DIR}/direct/dependency_probe"
if configure compiled -DPROBE_VENICE_DIRECT_TARGET=ON \
  -DPROBE_VENICE_EXPORTS="${exports[0]}" -DPROBE_VENICE_BAD=compiled \
  > "${WORK_DIR}/compiled.log" 2>&1; then
  echo "Compiled HTTP target unexpectedly passed" >&2
  exit 1
fi
if ! rg -q 'Venice transport contract failure' "${WORK_DIR}/compiled.log"; then
  cat "${WORK_DIR}/compiled.log" >&2
  exit 1
fi
configure preexisting
cmake --build "${WORK_DIR}/preexisting" --parallel 2
"${WORK_DIR}/preexisting/dependency_probe"
configure installed -DPROBE_VENICE_PREEXISTING=OFF
cmake --build "${WORK_DIR}/installed" --parallel 2
"${WORK_DIR}/installed/dependency_probe"
configure alternate-installed -DPROBE_VENICE_PREEXISTING=OFF \
  -Dvenice-cpp_DIR="${SOURCE_DIR}/cmake/dependency-probe/obsolete/lib/cmake/venice-cpp"
cmake --build "${WORK_DIR}/alternate-installed" --parallel 2
"${WORK_DIR}/alternate-installed/dependency_probe"
for kind in obsolete missing-target; do
  package_root="${WORK_DIR}/${kind}-package"
  package_dir="${package_root}/lib/cmake/venice-cpp"
  mkdir -p "${package_dir}"
  if [[ ${kind} == obsolete ]]; then
    cp "${SOURCE_DIR}/cmake/dependency-probe/obsolete/lib/cmake/venice-cpp/"* "${package_dir}/"
  else
    printf 'set(PACKAGE_VERSION "0.29.18")\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n' \
      > "${package_dir}/venice-cppConfigVersion.cmake"
    printf '# Deliberately missing canonical target\n' > "${package_dir}/venice-cppConfig.cmake"
  fi
  if configure "package-${kind}" -DPROBE_VENICE_PREEXISTING=OFF \
    -Dvenice-cpp_DIR="${package_dir}" \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE \
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE \
    -DCMAKE_FIND_ROOT_PATH="${package_root}" -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    > "${WORK_DIR}/package-${kind}.log" 2>&1; then
    echo "Invalid installed Venice package unexpectedly passed: ${kind}" >&2
    exit 1
  fi
  if ! rg -q 'canonical venice-cpp::lib target' "${WORK_DIR}/package-${kind}.log"; then
    cat "${WORK_DIR}/package-${kind}.log" >&2
    exit 1
  fi
done
mkdir -p "${WORK_DIR}/invalid-header"
printf '#error Deliberately invalid task-owned HTTP header\n' > "${WORK_DIR}/invalid-header/httplib.h"
# Inverse of bad-include below: a poisoned HTTP header beside the selected
# Venice guard must not replace the compatible canonical HTTP target's header.
# This is a gate-only configure fixture; the poisoned aggregate consumer include
# root is intentionally unsuitable for an application compile.
mkdir -p "${WORK_DIR}/ambient/venice/detail"
cp "${PREFIX}/include/venice/detail/httplib_contract.hpp" "${WORK_DIR}/ambient/venice/detail/"
cp "${WORK_DIR}/invalid-header/httplib.h" "${WORK_DIR}/ambient/httplib.h"
configure ambient-header -DPROBE_VENICE_BAD=ambient \
  -DPROBE_VENICE_AMBIENT_INCLUDE="${WORK_DIR}/ambient"
for kind in marker headers header_length line_length resolver exceptions tls tls_off links include; do
  if configure "bad-${kind}" -DPROBE_VENICE_BAD="${kind}" \
    -DPROBE_VENICE_HTTP_INCLUDE="${WORK_DIR}/invalid-header" \
    > "${WORK_DIR}/bad-${kind}.log" 2>&1; then
    echo "Incompatible selected Venice/HTTP target unexpectedly passed: ${kind}" >&2
    exit 1
  fi
  if ! rg -q 'Venice transport contract failure' "${WORK_DIR}/bad-${kind}.log"; then
    cat "${WORK_DIR}/bad-${kind}.log" >&2
    exit 1
  fi
done
mapfile -t headers < <(rg --files "${PREFIX}" | rg '/httplib\.h$')
if [[ ${#headers[@]} != 1 ]]; then
  echo "Expected one canonical HTTP header in installed fixture" >&2
  exit 1
fi
for kind in old_header missing_api; do
  mkdir -p "${WORK_DIR}/${kind}"
  if [[ ${kind} == old_header ]]; then
    sed 's/^#define CPPHTTPLIB_VERSION .*/#define CPPHTTPLIB_VERSION "0.18.3"/' \
      "${headers[0]}" > "${WORK_DIR}/${kind}/httplib.h"
  else
    sed 's/enable_system_ca/unsupported_system_ca/g' "${headers[0]}" \
      > "${WORK_DIR}/${kind}/httplib.h"
  fi
  if configure "${kind}" -DPROBE_VENICE_BAD=include \
    -DPROBE_VENICE_HTTP_INCLUDE="${WORK_DIR}/${kind}" \
    > "${WORK_DIR}/${kind}.log" 2>&1; then
    echo "Claimed-compatible HTTP target accepted ${kind}" >&2
    exit 1
  fi
  if ! rg -q 'Venice transport contract failure' "${WORK_DIR}/${kind}.log"; then
    cat "${WORK_DIR}/${kind}.log" >&2
    exit 1
  fi
done

for kind in headers include; do
  configure "reconfigure-${kind}" -DPROBE_VENICE_BAD=
  if configure "reconfigure-${kind}" -DPROBE_VENICE_BAD="${kind}" \
    -DPROBE_VENICE_HTTP_INCLUDE="${WORK_DIR}/invalid-header" \
    > "${WORK_DIR}/reconfigure-${kind}.log" 2>&1; then
    echo "Changed HTTP target reused stale successful proof: ${kind}" >&2
    exit 1
  fi
  if ! rg -q 'Venice transport contract failure' "${WORK_DIR}/reconfigure-${kind}.log"; then
    cat "${WORK_DIR}/reconfigure-${kind}.log" >&2
    exit 1
  fi
done
