#!/bin/sh
set -eu

version="${1:-0.1.5}"
root_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
build_dir="${root_dir}/build-win"
dist_dir="${root_dir}/dist"
package_name="rbdvbt_gui-windows-x64-${version}"
package_dir="${dist_dir}/${package_name}"
vendor_rtlsdr_dir="${dist_dir}/vendor/rtlsdr"

if [ ! -d "${package_dir}" ]; then
    echo "package directory not found: ${package_dir}" >&2
    echo "Create/stage the Qt runtime package once, then rerun this script." >&2
    exit 1
fi

install -m 755 "${build_dir}/rbdvbt_gui.exe" "${package_dir}/rbdvbt_gui.exe"
install -m 755 "${build_dir}/rbdvbt_rx.exe" "${package_dir}/rbdvbt_rx.exe"
install -m 644 "${root_dir}/README_WINDOWS_GUI.txt" "${package_dir}/README_WINDOWS_GUI.txt"
install -m 644 "${root_dir}/README.md" "${package_dir}/README.md"
install -m 644 "${root_dir}/README.md" "${package_dir}/README_PROJECT.md"
if [ -f "${root_dir}/RELEASE_WINDOWS_${version}.md" ]; then
    install -m 644 "${root_dir}/RELEASE_WINDOWS_${version}.md" "${package_dir}/RELEASE_WINDOWS_${version}.md"
fi

if [ -d "${vendor_rtlsdr_dir}" ]; then
    for file in rtl_sdr.exe librtlsdr.dll rtlsdr.dll libusb-1.0.dll; do
        if [ -f "${vendor_rtlsdr_dir}/${file}" ]; then
            install -m 755 "${vendor_rtlsdr_dir}/${file}" "${package_dir}/${file}"
        fi
    done
    if [ -f "${vendor_rtlsdr_dir}/SOURCE.txt" ]; then
        install -m 644 "${vendor_rtlsdr_dir}/SOURCE.txt" "${package_dir}/RTLSDR_SOURCE.txt"
    else
        {
            echo "Source: https://downloads.osmocom.org/binaries/windows/rtl-sdr/"
            echo "Package: rtl-sdr-64bit-20260517.zip"
            echo "Package URL: https://downloads.osmocom.org/binaries/windows/rtl-sdr/rtl-sdr-64bit-20260517.zip"
            echo "Architecture: 64-bit Windows"
            echo "Files copied into release package root when present: rtl_sdr.exe, librtlsdr.dll, rtlsdr.dll, libusb-1.0.dll"
        } > "${package_dir}/RTLSDR_SOURCE.txt"
    fi
    rm -f "${package_dir}/ADD_RTLSDR_FILES_HERE.txt"
fi

cd "${dist_dir}"
zip -rFS "${package_name}.zip" "${package_name}"
