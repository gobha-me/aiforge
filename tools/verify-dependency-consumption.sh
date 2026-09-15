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

for dependency_source in \
  catch2 cli11 miniaudio nlohmann_json rtaudio sqlite3_amalgamation aiforge_dbus1 yaml_cpp; do
  source_path="${SEED_BUILD}/_deps/${dependency_source}-src"
  if [[ -d "${source_path}" ]]; then
    variable_name=$(printf '%s' "${dependency_source}" | tr '[:lower:]-' '[:upper:]_')
    COMMON_CMAKE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${variable_name}=${source_path}")
  fi
done

if [[ -d "${SEED_BUILD}/_deps/c-ares-src" ]]; then
  COMMON_CMAKE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_C-ARES=${SEED_BUILD}/_deps/c-ares-src")
fi

"${SNAPSHOT_DIR}/tools/verify-systemd-journal-consumption.sh" "${COMMON_CMAKE_ARGS[@]}"

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
    rtaudio) printf 'RtAudio' ;;
    miniaudio) printf 'miniaudio' ;;
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

if [[ "$(uname -s)" == Linux ]]; then
  configure_probe fetched-dbus1 dbus1 \
    -DPROBE_EXPECT_EMBEDDED=ON -DCMAKE_DISABLE_FIND_PACKAGE_DBus1=TRUE
  build_and_run_probe fetched-dbus1
  dbus_build="${WORK_DIR}/fetched-dbus1/_deps/aiforge_dbus1-build"
  if [[ -e "${dbus_build}/bin/dbus-daemon" || -e "${dbus_build}/bin/dbus-send" ]]; then
    echo "Embedded libdbus unexpectedly built daemon tools" >&2
    exit 1
  fi
  cmake -DDBUS_BUILD_DIR="${dbus_build}" -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -P "${SNAPSHOT_DIR}/cmake/dependency-probe/install-dbus-client.cmake"
  configure_probe installed-dbus1 dbus1 -DPROBE_EXPECT_IMPORTED=ON \
    -DDBus1_DIR="${PREFIX}/lib/cmake/DBus1" -DFETCHCONTENT_FULLY_DISCONNECTED=ON
  build_and_run_probe installed-dbus1
  if configure_probe mismatched-dbus1 dbus1 \
    -DCMAKE_DISABLE_FIND_PACKAGE_DBus1=TRUE \
    -DFETCHCONTENT_SOURCE_DIR_AIFORGE_DBUS1="${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"; then
    echo "libdbus accepted a source without its client target" >&2
    exit 1
  fi
  # A rejected *_DIR hint may legitimately fall through to another compatible
  # installed package. Prove that behavior, then isolate the negative fixture
  # so a system DBus1 cannot accidentally turn it into a successful configure.
  configure_probe alternate-installed-dbus1 dbus1 -DPROBE_EXPECT_IMPORTED=ON \
    -DDBus1_DIR="${SNAPSHOT_DIR}/cmake/dependency-probe/obsolete/lib/cmake/DBus1" \
    -DCMAKE_PREFIX_PATH="${PREFIX}" -DFETCHCONTENT_FULLY_DISCONNECTED=ON
  build_and_run_probe alternate-installed-dbus1
  if configure_probe obsolete-dbus1 dbus1 \
    -DDBus1_DIR="${SNAPSHOT_DIR}/cmake/dependency-probe/obsolete/lib/cmake/DBus1" \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE \
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE \
    -DCMAKE_FIND_ROOT_PATH="${SNAPSHOT_DIR}/cmake/dependency-probe/obsolete" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DFETCHCONTENT_SOURCE_DIR_AIFORGE_DBUS1="${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"; then
    echo "libdbus accepted an obsolete package" >&2
    exit 1
  fi
  if configure_probe malformed-dbus1 dbus1 \
    -DDBus1_DIR="${SNAPSHOT_DIR}/cmake/dependency-probe/malformed/lib/cmake/DBus1" \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE \
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE \
    -DCMAKE_FIND_ROOT_PATH="${SNAPSHOT_DIR}/cmake/dependency-probe/malformed" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY; then
    echo "libdbus accepted missing client headers" >&2
    exit 1
  fi
fi

# Exercise the real upstream install/export path as well as our private
# fallback. No reconstructed successful package fixture can hide export bugs.
configure_probe "fetched-yaml_cpp" yaml_cpp \
  -DPROBE_EXPECT_EMBEDDED=ON -DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=TRUE
build_and_run_probe "fetched-yaml_cpp"
YAML_SOURCE="${WORK_DIR}/fetched-yaml_cpp/_deps/yaml_cpp-src"
if [[ ! -d "${YAML_SOURCE}" ]]; then
  YAML_SOURCE="${SEED_BUILD}/_deps/yaml_cpp-src"
fi
cmake -S "${YAML_SOURCE}" -B "${WORK_DIR}/install-yaml_cpp" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -DYAML_CPP_BUILD_TESTS=OFF -DYAML_CPP_BUILD_TOOLS=OFF \
  -DYAML_CPP_BUILD_CONTRIB=OFF -DYAML_CPP_FORMAT_SOURCE=OFF \
  -DYAML_CPP_INSTALL=ON -DYAML_CPP_DISABLE_UNINSTALL=ON \
  -DYAML_BUILD_SHARED_LIBS=OFF -DYAML_ENABLE_PIC=ON \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"
cmake --build "${WORK_DIR}/install-yaml_cpp" --parallel 2
cmake --install "${WORK_DIR}/install-yaml_cpp"
configure_probe "installed-yaml_cpp" yaml_cpp \
  -DCMAKE_PREFIX_PATH="${PREFIX}" -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  -DPROBE_EXPECT_IMPORTED=ON
build_and_run_probe "installed-yaml_cpp"
if [[ -d "${WORK_DIR}/installed-yaml_cpp/_deps/yaml_cpp-src" ]]; then
  echo "Installed yaml-cpp consumer unexpectedly fetched its dependency" >&2
  exit 1
fi
if configure_probe "mismatched-yaml_cpp" yaml_cpp \
  -DCMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=TRUE \
  -DFETCHCONTENT_SOURCE_DIR_YAML_CPP="${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"; then
  echo "yaml-cpp accepted a fallback without its canonical target" >&2
  exit 1
fi
for package_case in obsolete missing-target; do
  PACKAGE_DIR="${WORK_DIR}/${package_case}-yaml-package/lib/cmake/yaml-cpp"
  mkdir -p "${PACKAGE_DIR}"
  if [[ "${package_case}" == obsolete ]]; then
    cat > "${PACKAGE_DIR}/yaml-cpp-config-version.cmake" <<'EOF'
set(PACKAGE_VERSION "0.8.0")
set(PACKAGE_VERSION_COMPATIBLE FALSE)
EOF
  else
    cat > "${PACKAGE_DIR}/yaml-cpp-config-version.cmake" <<'EOF'
set(PACKAGE_VERSION "0.9.0")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
EOF
  fi
  cat > "${PACKAGE_DIR}/yaml-cpp-config.cmake" <<'EOF'
set(yaml-cpp_FOUND TRUE)
EOF
  if configure_probe "${package_case}-yaml_cpp" yaml_cpp \
    -Dyaml-cpp_DIR="${PACKAGE_DIR}" \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE \
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE \
    -DCMAKE_FIND_ROOT_PATH="${WORK_DIR}/${package_case}-yaml-package" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY \
    -DFETCHCONTENT_SOURCE_DIR_YAML_CPP="${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"; then
    echo "yaml-cpp accepted ${package_case} package evidence" >&2
    exit 1
  fi
done

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
VENICE_CARES_SOURCE="${VENICE_BUILD}/_deps/c-ares-src"
if [[ ! -d "${VENICE_HTTPLIB_SOURCE}" ]]; then
  VENICE_HTTPLIB_SOURCE="${SEED_BUILD}/_deps/httplib-src"
fi
if [[ ! -d "${VENICE_NLOHMANN_SOURCE}" ]]; then
  VENICE_NLOHMANN_SOURCE="${SEED_BUILD}/_deps/nlohmann_json-src"
fi
VENICE_CARES_ARGS=()
if [[ -d "${VENICE_CARES_SOURCE}" ]]; then
  VENICE_CARES_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_C-ARES=${VENICE_CARES_SOURCE}")
elif [[ -d "${SEED_BUILD}/_deps/c-ares-src" ]]; then
  VENICE_CARES_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_C-ARES=${SEED_BUILD}/_deps/c-ares-src")
fi
cmake -S "${VENICE_SOURCE}" -B "${WORK_DIR}/install-venice-cpp" \
  "${COMMON_CMAKE_ARGS[@]}" \
  "${VENICE_CARES_ARGS[@]}" \
  -Dvenice-cpp_BUILD_BIN=OFF -Dvenice-cpp_TESTS=OFF -Dvenice-cpp_INSTALL=ON \
  -DFETCHCONTENT_SOURCE_DIR_HTTPLIB="${VENICE_HTTPLIB_SOURCE}" \
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="${VENICE_NLOHMANN_SOURCE}" \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"
cmake --build "${WORK_DIR}/install-venice-cpp" --parallel 2
cmake --install "${WORK_DIR}/install-venice-cpp"
"${SNAPSHOT_DIR}/tools/verify-venice-transport-consumption.sh" "${PREFIX}" "${COMMON_CMAKE_ARGS[@]}"

cmake -S "${WORK_DIR}/fetched-rasterforge/_deps/rasterforge-src" \
  -B "${WORK_DIR}/install-rasterforge" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Drasterforge_BUILD_LIB=ON -Drasterforge_BUILD_BIN=OFF \
  -Drasterforge_TESTS=OFF -Drasterforge_FUZZERS=OFF \
  -Drasterforge_BENCHMARKS=OFF -Drasterforge_INSTALL=ON \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"
cmake --build "${WORK_DIR}/install-rasterforge" --parallel 2
cmake --install "${WORK_DIR}/install-rasterforge"

INCOMPLETE_TERMFORGE_PREFIX="${WORK_DIR}/incomplete-termforge"
cmake -E copy_directory "${PREFIX}" "${INCOMPLETE_TERMFORGE_PREFIX}"
cmake -E remove \
  "${INCOMPLETE_TERMFORGE_PREFIX}/include/termforge/widgets/choice_wizard_dialog.hpp"
INCOMPLETE_TERMFORGE_LOG="${WORK_DIR}/incomplete-termforge.log"
if configure_probe "incomplete-termforge" termforge \
  -DCMAKE_PREFIX_PATH="${INCOMPLETE_TERMFORGE_PREFIX}" \
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
  >"${INCOMPLETE_TERMFORGE_LOG}" 2>&1; then
  echo "termforge accepted an installed package with missing headers" >&2
  exit 1
fi
if ! grep -Fq "TermForge package contract failure" \
    "${INCOMPLETE_TERMFORGE_LOG}" || \
  ! grep -Fq "termforge/widgets/choice_wizard_dialog.hpp" \
    "${INCOMPLETE_TERMFORGE_LOG}"; then
  cat "${INCOMPLETE_TERMFORGE_LOG}" >&2
  echo "termforge missing-header diagnostic was not actionable" >&2
  exit 1
fi

for dependency in termforge venice_cpp rasterforge; do
  fetch_name=$(dependency_fetch_name "${dependency}")
  configure_probe "installed-${dependency}" "${dependency}" \
    -DPROBE_EXPECT_IMPORTED=ON \
    -DCMAKE_PREFIX_PATH="${PREFIX}" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
  build_and_run_probe "installed-${dependency}"

  if configure_probe "mismatched-${dependency}" "${dependency}" \
    "-DCMAKE_DISABLE_FIND_PACKAGE_${fetch_name}=TRUE" \
    "-DFETCHCONTENT_SOURCE_DIR_${fetch_name^^}=${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"; then
    echo "${dependency} accepted a dependency without its canonical target" >&2
    exit 1
  fi

  # Venice's isolated negative and valid installed-alternative cases run in
  # verify-venice-transport-consumption.sh above.
  if [[ ${dependency} != venice_cpp ]] && configure_probe "obsolete-${dependency}" "${dependency}" \
    -DCMAKE_PREFIX_PATH="${SNAPSHOT_DIR}/cmake/dependency-probe/obsolete" \
    "-DFETCHCONTENT_SOURCE_DIR_${fetch_name^^}=${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"; then
    echo "${dependency} accepted an obsolete installed package" >&2
    exit 1
  fi
done

cmake -E create_symlink \
  "${WORK_DIR}/fetched-termforge/_deps/termforge-src" \
  "${SNAPSHOT_DIR}/cmake/termforge"
configure_probe "sibling-termforge" termforge \
  -DPROBE_EXPECT_EMBEDDED=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_termforge=TRUE
build_and_run_probe "sibling-termforge"

cmake -E create_symlink "${VENICE_SOURCE}" "${SNAPSHOT_DIR}/cmake/venice-cpp"
configure_probe "sibling-venice" venice_cpp \
  -DPROBE_EXPECT_EMBEDDED=ON -DCMAKE_DISABLE_FIND_PACKAGE_venice-cpp=TRUE \
  -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -DFETCHCONTENT_SOURCE_DIR_VENICE-CPP="${SNAPSHOT_DIR}/cmake/dependency-probe/mismatched"
build_and_run_probe "sibling-venice"
cmake -E rm "${SNAPSHOT_DIR}/cmake/venice-cpp"
mkdir -p "${SNAPSHOT_DIR}/cmake/venice-cpp"
cat > "${SNAPSHOT_DIR}/cmake/venice-cpp/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.28)
project(obsolete_sibling LANGUAGES CXX)
add_library(venice-cpp::lib INTERFACE IMPORTED GLOBAL)
EOF
if configure_probe "stale-sibling-venice" venice_cpp \
  -DCMAKE_DISABLE_FIND_PACKAGE_venice-cpp=TRUE \
  > "${WORK_DIR}/stale-sibling-venice.log" 2>&1; then
  echo "Incompatible canonical sibling unexpectedly passed" >&2
  exit 1
fi
if ! grep -Eq 'Venice transport contract failure' "${WORK_DIR}/stale-sibling-venice.log"; then
  cat "${WORK_DIR}/stale-sibling-venice.log" >&2
  exit 1
fi
cmake -E remove_directory "${SNAPSHOT_DIR}/cmake/venice-cpp"

cmake -S "${SNAPSHOT_DIR}/cmake/dependency-probe" \
  -B "${WORK_DIR}/preexisting" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -DAIFORGE_SOURCE_DIR="${SNAPSHOT_DIR}" \
  -DPROBE_TERMFORGE_INCLUDE_DIR="${WORK_DIR}/fetched-termforge/_deps/termforge-src/include" \
  -DPROBE_PREEXISTING_TARGETS=ON \
  -DCMAKE_PREFIX_PATH="${PREFIX}"

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-fetched" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -Daiforge_AUDIO_PLAYBACK=OFF \
  -Daiforge_AUDIO_CAPTURE=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_termforge=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_venice-cpp=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_rasterforge=TRUE

if [[ ! -d "${WORK_DIR}/aiforge-fetched/_deps/rasterforge-src" ]]; then
  echo "Ordinary adapter build did not activate RasterForge" >&2
  exit 1
fi

for dependency in miniaudio rtaudio; do
  if [[ -d "${WORK_DIR}/aiforge-fetched/_deps/${dependency}-src" ]]; then
    echo "Ordinary adapter build unexpectedly activated ${dependency}" >&2
    exit 1
  fi
done

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-installed" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -DCMAKE_PREFIX_PATH="${PREFIX}" \
  -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -Daiforge_AUDIO_PLAYBACK=OFF \
  -Daiforge_AUDIO_CAPTURE=OFF

for dependency in termforge venice-cpp rasterforge aiforge_dbus1 yaml_cpp; do
  if [[ -d "${WORK_DIR}/aiforge-installed/_deps/${dependency}-src" ]]; then
    echo "Installed AIForge consumer unexpectedly fetched ${dependency}" >&2
    exit 1
  fi
done

for dependency in miniaudio rtaudio; do
  if [[ -d "${WORK_DIR}/aiforge-installed/_deps/${dependency}-src" ]]; then
    echo "Installed AIForge consumer unexpectedly activated ${dependency}" >&2
    exit 1
  fi
done

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-core" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Daiforge_BUILD_ADAPTERS=OFF -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_SystemdJournal=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_termforge=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_venice-cpp=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_rasterforge=TRUE

if ! grep -Fxq "aiforge_AUDIO_PLAYBACK:BOOL=OFF" \
  "${WORK_DIR}/aiforge-core/CMakeCache.txt"; then
  echo "Top-level adapter-free configure unexpectedly enabled playback" >&2
  exit 1
fi
if ! grep -Fxq "aiforge_AUDIO_CAPTURE:BOOL=OFF" \
  "${WORK_DIR}/aiforge-core/CMakeCache.txt"; then
  echo "Top-level adapter-free configure unexpectedly enabled capture" >&2
  exit 1
fi

for dependency in termforge venice-cpp rasterforge aiforge_dbus1 yaml_cpp; do
  if [[ -d "${WORK_DIR}/aiforge-core/_deps/${dependency}-src" ]]; then
    echo "Core-only AIForge unexpectedly activated ${dependency}" >&2
    exit 1
  fi
done

if [[ -d "${WORK_DIR}/aiforge-core/_deps/aiforge_dbus1-build" ]]; then
  echo "Core-only AIForge unexpectedly configured the libdbus client" >&2
  exit 1
fi

for dependency in miniaudio rtaudio; do
  if [[ -d "${WORK_DIR}/aiforge-core/_deps/${dependency}-src" ]]; then
    echo "Core-only AIForge unexpectedly activated ${dependency}" >&2
    exit 1
  fi
done

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-audio-device-evaluation" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Daiforge_AUDIO_DEVICE_EVALUATION=ON \
  -Daiforge_AUDIO_PLAYBACK=OFF \
  -Daiforge_AUDIO_CAPTURE=OFF \
  -Daiforge_BUILD_ADAPTERS=OFF -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_SystemdJournal=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_miniaudio=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_RtAudio=TRUE

AUDIO_DEVICE_CACHE="${WORK_DIR}/aiforge-audio-device-evaluation/CMakeCache.txt"
for expected in \
  "AIFORGE_MINIAUDIO_DEPENDENCY_SOURCE:INTERNAL=controlled_source_fallback" \
  "AIFORGE_RTAUDIO_DEPENDENCY_SOURCE:INTERNAL=controlled_source_fallback" \
  "MINIAUDIO_ENABLE_ALSA:BOOL=ON" \
  "MINIAUDIO_ENABLE_NULL:BOOL=ON" \
  "MINIAUDIO_ENABLE_ONLY_SPECIFIC_BACKENDS:BOOL=ON" \
  "MINIAUDIO_ENABLE_PULSEAUDIO:BOOL=ON" \
  "MINIAUDIO_NO_DECODING:BOOL=ON" \
  "MINIAUDIO_NO_ENCODING:BOOL=ON" \
  "MINIAUDIO_NO_RUNTIME_LINKING:BOOL=OFF" \
  "RTAUDIO_API_ALSA:BOOL=ON" \
  "RTAUDIO_API_JACK:BOOL=OFF" \
  "RTAUDIO_API_PULSE:BOOL=OFF" \
  "RTAUDIO_BUILD_SHARED_LIBS:BOOL=OFF" \
  "RTAUDIO_BUILD_TESTING:BOOL=OFF"; do
  if ! grep -Fxq "${expected}" "${AUDIO_DEVICE_CACHE}"; then
    echo "Audio-device evaluation dependency contract drifted: ${expected}" >&2
    exit 1
  fi
done

# Building the named targets is the activation proof. Their source directories
# may live outside this temporary tree when COMMON_CMAKE_ARGS seeds an existing
# verified download through FETCHCONTENT_SOURCE_DIR_*.
cmake --build "${WORK_DIR}/aiforge-audio-device-evaluation" \
  --target miniaudio rtaudio --parallel 2

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-audio-playback" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Daiforge_AUDIO_PLAYBACK=ON \
  -Daiforge_AUDIO_CAPTURE=OFF \
  -Daiforge_AUDIO_DEVICE_EVALUATION=OFF \
  -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_RtAudio=TRUE

PLAYBACK_CACHE="${WORK_DIR}/aiforge-audio-playback/CMakeCache.txt"
for expected in \
  "AIFORGE_RTAUDIO_DEPENDENCY_SOURCE:INTERNAL=controlled_source_fallback" \
  "RTAUDIO_API_ALSA:BOOL=ON" \
  "RTAUDIO_API_JACK:BOOL=OFF" \
  "RTAUDIO_API_PULSE:BOOL=OFF"; do
  if ! grep -Fxq "${expected}" "${PLAYBACK_CACHE}"; then
    echo "Production audio-playback dependency contract drifted: ${expected}" >&2
    exit 1
  fi
done
if [[ -d "${WORK_DIR}/aiforge-audio-playback/_deps/miniaudio-src" ]]; then
  echo "Production audio playback unexpectedly activated miniaudio" >&2
  exit 1
fi
cmake --build "${WORK_DIR}/aiforge-audio-playback" \
  --target aiforge_adapters --parallel 2

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-audio-capture" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Daiforge_AUDIO_PLAYBACK=OFF \
  -Daiforge_AUDIO_CAPTURE=ON \
  -Daiforge_AUDIO_DEVICE_EVALUATION=OFF \
  -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_RtAudio=TRUE

CAPTURE_CACHE="${WORK_DIR}/aiforge-audio-capture/CMakeCache.txt"
for expected in \
  "AIFORGE_RTAUDIO_DEPENDENCY_SOURCE:INTERNAL=controlled_source_fallback" \
  "RTAUDIO_API_ALSA:BOOL=ON" \
  "RTAUDIO_API_JACK:BOOL=OFF" \
  "RTAUDIO_API_PULSE:BOOL=OFF"; do
  if ! grep -Fxq "${expected}" "${CAPTURE_CACHE}"; then
    echo "Production audio-capture dependency contract drifted: ${expected}" >&2
    exit 1
  fi
done
if [[ -d "${WORK_DIR}/aiforge-audio-capture/_deps/miniaudio-src" ]]; then
  echo "Production audio capture unexpectedly activated miniaudio" >&2
  exit 1
fi
cmake --build "${WORK_DIR}/aiforge-audio-capture" \
  --target aiforge_adapters --parallel 2

cmake -S "${SNAPSHOT_DIR}" -B "${WORK_DIR}/aiforge-audio-both" \
  "${COMMON_CMAKE_ARGS[@]}" \
  -Daiforge_AUDIO_PLAYBACK=ON \
  -Daiforge_AUDIO_CAPTURE=ON \
  -Daiforge_AUDIO_DEVICE_EVALUATION=ON \
  -Daiforge_BUILD_BIN=OFF -Daiforge_TESTS=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_miniaudio=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_RtAudio=TRUE
cmake --build "${WORK_DIR}/aiforge-audio-both" \
  --target aiforge_adapters aiforge_audio_device_rtaudio_probe --parallel 2
