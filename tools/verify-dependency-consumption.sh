#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SEED_BUILD=${1:-}

if [[ -z "${SEED_BUILD}" || ! -f "${SEED_BUILD}/CMakeCache.txt" ]]; then
  echo "usage: $0 <configured-aiforge-build-dir>" >&2
  exit 2
fi

SEED_BUILD=$(cd "${SEED_BUILD}" && pwd)
WORK_DIR=$(mktemp -d)
trap 'cmake -E remove_directory "${WORK_DIR}"' EXIT

SNAPSHOT_DIR="${WORK_DIR}/aiforge-source"
mkdir -p "${SNAPSHOT_DIR}"
git -C "${SOURCE_DIR}" ls-files --cached --others --exclude-standard -z \
  | tar -C "${SOURCE_DIR}" --null --files-from=- --create --file=- \
  | tar --extract --file=- -C "${SNAPSHOT_DIR}"

TOOLCHAIN=$(sed -n 's/^CMAKE_TOOLCHAIN_FILE:[^=]*=//p' \
  "${SEED_BUILD}/CMakeCache.txt")
COMMON_CMAKE_ARGS=()
if [[ -n "${TOOLCHAIN}" ]]; then
  if [[ "${TOOLCHAIN}" != /* ]]; then
    TOOLCHAIN="${SNAPSHOT_DIR}/${TOOLCHAIN}"
  fi
  COMMON_CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}")
fi

for dependency_source in catch2 cli11 nlohmann_json sqlite3_amalgamation; do
  source_path="${SEED_BUILD}/_deps/${dependency_source}-src"
  if [[ -d "${source_path}" ]]; then
    variable_name=$(printf '%s' "${dependency_source}" | tr '[:lower:]-' '[:upper:]_')
    COMMON_CMAKE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${variable_name}=${source_path}")
  fi
done

configure_probe() {
  local name=$1
  local dependency=$2
  shift 2
  cmake -S "${SNAPSHOT_DIR}/cmake/dependency-probe" \
    -B "${WORK_DIR}/${name}" \
    "${COMMON_CMAKE_ARGS[@]}" \
    -DAIFORGE_SOURCE_DIR="${SNAPSHOT_DIR}" \
    -DPROBE_DEPENDENCY="${dependency}" \
    "$@"
}

build_and_run_probe() {
  local name=$1
  cmake --build "${WORK_DIR}/${name}" --parallel 2
  "${WORK_DIR}/${name}/dependency_probe"
}

dependency_fetch_name() {
  case "$1" in
    termforge) printf 'termforge' ;;
    venice_cpp) printf 'venice-cpp' ;;
    rasterforge) printf 'rasterforge' ;;
  esac
}

for dependency in termforge venice_cpp rasterforge; do
  fetch_name=$(dependency_fetch_name "${dependency}")
  configure_probe "fetched-${dependency}" "${dependency}" \
    -DPROBE_EXPECT_EMBEDDED=ON \
    "-DCMAKE_DISABLE_FIND_PACKAGE_${fetch_name}=TRUE"
  build_and_run_probe "fetched-${dependency}"
done

configure_probe "system-sqlite3" sqlite3
build_and_run_probe "system-sqlite3"

configure_probe "fetched-sqlite3" sqlite3 \
  -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=TRUE
build_and_run_probe "fetched-sqlite3"

PREFIX="${WORK_DIR}/installed"

cmake -S "${WORK_DIR}/fetched-termforge/_deps/termforge-src" \
  -B "${WORK_DIR}/install-termforge" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Dtermforge_TESTS=OFF -Dtermforge_EXAMPLES=OFF -Dtermforge_BIN=OFF \
  -Dtermforge_TOOLS=OFF -Dtermforge_BENCH=OFF -Dtermforge_INSTALL=ON \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"
cmake --build "${WORK_DIR}/install-termforge" --parallel 2
cmake --install "${WORK_DIR}/install-termforge"

VENICE_SOURCE="${WORK_DIR}/fetched-venice_cpp/_deps/venice-cpp-src"
VENICE_BUILD="${WORK_DIR}/fetched-venice_cpp"
VENICE_HTTPLIB_SOURCE="${VENICE_BUILD}/_deps/httplib-src"
VENICE_NLOHMANN_SOURCE="${VENICE_BUILD}/_deps/nlohmann_json-src"
if [[ ! -d "${VENICE_HTTPLIB_SOURCE}" ]]; then
  VENICE_HTTPLIB_SOURCE="${SEED_BUILD}/_deps/httplib-src"
fi
if [[ ! -d "${VENICE_NLOHMANN_SOURCE}" ]]; then
  VENICE_NLOHMANN_SOURCE="${SEED_BUILD}/_deps/nlohmann_json-src"
fi
cmake -S "${VENICE_SOURCE}" -B "${WORK_DIR}/install-venice-cpp" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Dvenice-cpp_BUILD_BIN=OFF -Dvenice-cpp_TESTS=OFF -Dvenice-cpp_INSTALL=ON \
  -DFETCHCONTENT_SOURCE_DIR_HTTPLIB="${VENICE_HTTPLIB_SOURCE}" \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="${VENICE_NLOHMANN_SOURCE}" \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"
cmake --build "${WORK_DIR}/install-venice-cpp" --parallel 2
cmake --install "${WORK_DIR}/install-venice-cpp"

cmake -S "${WORK_DIR}/fetched-rasterforge/_deps/rasterforge-src" \
  -B "${WORK_DIR}/install-rasterforge" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Drasterforge_BUILD_LIB=ON -Drasterforge_BUILD_BIN=OFF \
  -Drasterforge_TESTS=OFF -Drasterforge_FUZZERS=OFF \
  -Drasterforge_BENCHMARKS=OFF -Drasterforge_INSTALL=ON \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"
cmake --build "${WORK_DIR}/install-rasterforge" --parallel 2
cmake --install "${WORK_DIR}/install-rasterforge"

for dependency in termforge venice_cpp rasterforge; do
  fetch_name=$(dependency_fetch_name "${dependency}")
  configure_probe "installed-${dependency}" "${dependency}" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
  build_and_run_probe "installed-${dependency}"

  if configure_probe "mismatched-${dependency}" "${dependency}" \
    "-DCMAKE_DISABLE_FIND_PACKAGE_${fetch_name}=TRUE" \
    "-DFETCHCONTENT_SOURCE_DIR_${fetch_name^^}=${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"; then
    echo "${dependency} accepted a dependency without its canonical target" >&2
    exit 1
  fi

  if configure_probe "obsolete-${dependency}" "${dependency}" \
    -DCMAKE_PREFIX_PATH="${SNAPSHOT_DIR}/cmake/dependency-probe/obsolete" \
    "-DFETCHCONTENT_SOURCE_DIR_${fetch_name^^}=${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"; then
    echo "${dependency} accepted an obsolete installed package" >&2
    exit 1
  fi
done

cmake -S "${SNAPSHOT_DIR}/cmake/dependency-probe" \
  -B "${WORK_DIR}/preexisting" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -DAIFORGE_SOURCE_DIR="${SNAPSHOT_DIR}" \
  -DPROBE_PREEXISTING_TARGETS=ON

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-fetched" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_termforge=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_venice-cpp=TRUE

if [[ -d "${WORK_DIR}/aiforge-fetched/_deps/rasterforge-src" ]]; then
  echo "RasterForge was activated by the ordinary AIForge build" >&2
  exit 1
fi

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-installed" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF

for dependency in termforge venice-cpp rasterforge; do
  if [[ -d "${WORK_DIR}/aiforge-installed/_deps/${dependency}-src" ]]; then
    echo "Installed AIForge consumer unexpectedly fetched ${dependency}" >&2
    exit 1
  fi
done

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-core" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Daiforge_BUILD_ADAPTERS=OFF -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_termforge=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_venice-cpp=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_rasterforge=TRUE

for dependency in termforge venice-cpp rasterforge; do
  if [[ -d "${WORK_DIR}/aiforge-core/_deps/${dependency}-src" ]]; then
    echo "Core-only AIForge unexpectedly activated ${dependency}" >&2
    exit 1
  fi
done
