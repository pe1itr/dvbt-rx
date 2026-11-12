#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_dir="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
build_type="${BUILD_TYPE:-Release}"
jobs="${JOBS:-2}"
install_deps="${INSTALL_DEPS:-0}"
with_x11="${WITH_X11:-0}"
target_cpu="${TARGET_CPU:-auto}"
run_tests="${RUN_TESTS:-0}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--install-deps] [--clean]

Environment:
  BUILD_DIR    Build directory. Default: ${repo_dir}/build
  BUILD_TYPE   CMake build type. Default: Release
  JOBS         Parallel build jobs. Default: 2 (limits build memory usage)
  TARGET_CPU   auto, generic or odroid-n2. Default: auto (N2 on aarch64)
  WITH_X11     Build decoder X11 diagnostics and install headers when set to 1
  RUN_TESTS    Run CTest after building when set to 1. Default: 0

Examples:
  tools-pi6ehv/build_odroid_arm64.sh
  INSTALL_DEPS=1 WITH_X11=1 tools-pi6ehv/build_odroid_arm64.sh
  BUILD_DIR=build-odroid JOBS=4 tools-pi6ehv/build_odroid_arm64.sh
EOF
}

clean=0
for arg in "$@"; do
    case "${arg}" in
        --install-deps) install_deps=1 ;;
        --clean) clean=1 ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf "Unknown argument: %s\n\n" "${arg}" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "${target_cpu}" in
    auto)
        case "$(uname -m)" in
            aarch64|arm64) target_cpu=odroid-n2 ;;
            *) target_cpu=generic ;;
        esac
        ;;
    generic|odroid-n2) ;;
    *) printf "Unknown TARGET_CPU: %s\n" "${target_cpu}" >&2; exit 2 ;;
esac
case "${jobs}" in
    ""|*[!0-9]*|0) printf "JOBS must be a positive integer\n" >&2; exit 2 ;;
esac
case "${with_x11}" in
    0) cmake_x11=OFF ;;
    1) cmake_x11=ON ;;
    *) printf "WITH_X11 must be 0 or 1\n" >&2; exit 2 ;;
esac

if [ "${install_deps}" = "1" ]; then
    packages="build-essential cmake pkg-config python3 libfftw3-dev libglib2.0-dev rtl-sdr ffmpeg"
    if [ "${with_x11}" = "1" ]; then
        packages="${packages} libx11-dev"
    fi
    sudo apt-get update
    sudo apt-get install -y ${packages}
fi

if [ "${clean}" = "1" ]; then
    cmake --build "${build_dir}" --target clean 2>/dev/null || true
fi

cmake -S "${repo_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${build_type}" \
    -DRBDVBT_BUILD_GUI=OFF -DRBDVBT_BUILD_SRT=ON \
    -DRBDVBT_WITH_X11="${cmake_x11}" -DRBDVBT_TARGET_CPU="${target_cpu}" -DBUILD_TESTING=ON
cmake --build "${build_dir}" -j "${jobs}"
if [ "${run_tests}" = "1" ]; then
    ctest --test-dir "${build_dir}" --output-on-failure
fi

printf "\nBuilt receiver:\n  %s/rbdvbt_rx\n" "${build_dir}"
printf "CPU tuning: %s; decoder X11: %s\n" "${target_cpu}" "${cmake_x11}"
if [ -x "${build_dir}/rbdvbt_status_watch" ]; then
    printf "Status monitor:\n  %s/rbdvbt_status_watch\n" "${build_dir}"
fi
