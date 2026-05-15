#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_dir="$(CDPATH= cd -- "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_dir}/build}"
build_type="${BUILD_TYPE:-Release}"
jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf "4")}"
install_deps="${INSTALL_DEPS:-0}"
with_x11="${WITH_X11:-0}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--install-deps] [--clean]

Environment:
  BUILD_DIR    Build directory. Default: ${repo_dir}/build
  BUILD_TYPE   CMake build type. Default: Release
  JOBS         Parallel build jobs. Default: CPU count
  WITH_X11     Install optional X11 headers with --install-deps when set to 1

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

if [ "${install_deps}" = "1" ]; then
    packages="build-essential cmake pkg-config libfftw3-dev rtl-sdr ffmpeg"
    if [ "${with_x11}" = "1" ]; then
        packages="${packages} libx11-dev"
    fi
    sudo apt-get update
    sudo apt-get install -y ${packages}
fi

if [ "${clean}" = "1" ]; then
    cmake --build "${build_dir}" --target clean 2>/dev/null || true
fi

cmake -S "${repo_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE="${build_type}"
cmake --build "${build_dir}" -j "${jobs}"

printf "\nBuilt receiver:\n  %s/rbdvbt_rx\n" "${build_dir}"
if [ -x "${build_dir}/rbdvbt_status_watch" ]; then
    printf "Status monitor:\n  %s/rbdvbt_status_watch\n" "${build_dir}"
fi
