#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_dir="$(CDPATH= cd -- "${script_dir}/.." && pwd)"

build_dir="${BUILD_DIR:-${repo_dir}/build}"
rbdvbt_rx="${RBDVBT_RX:-${build_dir}/rbdvbt_rx}"
rtl_sdr_bin="${RTL_SDR:-rtl_sdr}"
ffmpeg_bin="${FFMPEG:-ffmpeg}"

frequency="${FREQUENCY:-436000000}"
device="${RTL_DEVICE:-00000001}"
sample_rate="${SAMPLERATE:-1010526}"
symbol_rate="${SYMBOLRATE:-333000}"
gain="${GAIN:-30}"
gi="${GI:-1/32}"
fec="${FEC:-2/3}"
live_symbols="${LIVE_SYMBOLS:-64}"
probe_symbols="${PROBE_SYMBOLS:-64}"
input_format="${INPUT_FORMAT:-u8}"
status_json="${STATUS_JSON:-/var/www/html/dvb/dvbt-rx-status.json}"
loglevel="${LOGLEVEL:-quiet}"
session="${SESSION:-$(date +%Y%m%d_%H%M%S)}"
log_dir="${LOG_DIR:-${repo_dir}/logs}"
rxlog="${RXLOG:-${log_dir}/rx_${session}.log}"
srt_url="${SRT_URL:-srt://44.137.26.85:4001?mode=caller&latency=500000}"
ffmpeg_loglevel="${FFMPEG_LOGLEVEL:-warning}"
video_start="${WAIT_VIDEO_START:-1}"
enable_gui="${GUI:-0}"
udp_out="${UDP_OUT:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0")

Start rtl_sdr -> rbdvbt_rx -> ffmpeg -> SRT.

Environment:
  FREQUENCY       RTL-SDR tune frequency in Hz. Default: ${frequency}
  RTL_DEVICE      RTL-SDR device index or serial for -d. Default: ${device}
  SAMPLERATE      RTL-SDR and receiver sample rate. Default: ${sample_rate}
  SYMBOLRATE      DVB-T symbol rate. Default: ${symbol_rate}
  GAIN            RTL-SDR gain. Default: ${gain}
  GI              DVB-T guard interval. Default: ${gi}
  FEC             DVB-T FEC. Default: ${fec}
  SRT_URL         Destination URL. Default: ${srt_url}
  FFMPEG          ffmpeg binary path. Default: ffmpeg
  RTL_SDR         rtl_sdr binary path. Default: rtl_sdr
  RBDVBT_RX       receiver path. Default: ${build_dir}/rbdvbt_rx
  BUILD_DIR       build directory. Default: ${build_dir}
  STATUS_JSON     receiver status JSON. Default: ${status_json}
  LOGLEVEL        receiver loglevel. Default: ${loglevel}
  LOG_DIR         log directory. Default: ${log_dir}
  RXLOG           receiver log path. Default: ${rxlog}
  GUI             pass --gui to rbdvbt_rx when set to 1. Default: 0
  UDP_OUT         optional extra UDP TS output, for example 127.0.0.1:10000
  WAIT_VIDEO_START wait for a clean video start when set to 1. Default: 1

Example:
  SRT_URL='srt://44.137.26.85:4001?mode=caller&latency=500000' \\
    tools-pi6ehv/dvbt_rx_pi6ehv.sh
EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
esac

if [ ! -x "${rbdvbt_rx}" ]; then
    printf "Receiver not found or not executable: %s\n" "${rbdvbt_rx}" >&2
    printf "Build it first with: tools-pi6ehv/build_odroid_arm64.sh\n" >&2
    exit 1
fi
if ! command -v "${rtl_sdr_bin}" >/dev/null 2>&1; then
    printf "rtl_sdr not found: %s\n" "${rtl_sdr_bin}" >&2
    exit 1
fi
if ! command -v "${ffmpeg_bin}" >/dev/null 2>&1; then
    printf "ffmpeg not found: %s\n" "${ffmpeg_bin}" >&2
    exit 1
fi

mkdir -p "${log_dir}"

rx_args=(
    --stdin
    --live
    --input-format "${input_format}"
    --sample-rate "${sample_rate}"
    --sr "${symbol_rate}"
    --gi "${gi}"
    --fec "${fec}"
    --resample-to-dvbt-rate
    --live-symbols "${live_symbols}"
    --probe-symbols "${probe_symbols}"
    --stdout-ts
    --status-json "${status_json}"
    --loglevel "${loglevel}"
)

if [ "${video_start}" = "1" ]; then
    rx_args+=(--wait-video-start)
fi
if [ "${enable_gui}" = "1" ]; then
    rx_args+=(--gui)
fi
if [ -n "${udp_out}" ]; then
    rx_args+=(--udp-out "${udp_out}")
fi

printf "Starting RTL-SDR receiver to SRT\n" >&2
printf "  RF:        %s Hz, sample rate %s, gain %s, device %s\n" "${frequency}" "${sample_rate}" "${gain}" "${device}" >&2
printf "  DVB-T:     SR %s, GI %s, FEC %s\n" "${symbol_rate}" "${gi}" "${fec}" >&2
printf "  SRT:       %s\n" "${srt_url}" >&2
printf "  RX log:    %s\n" "${rxlog}" >&2
printf "  Status:    %s\n" "${status_json}" >&2

"${rtl_sdr_bin}" -d "${device}" -f "${frequency}" -s "${sample_rate}" -g "${gain}" - \
    | "${rbdvbt_rx}" "${rx_args[@]}" 2> "${rxlog}" \
    | "${ffmpeg_bin}" \
        -hide_banner \
        -loglevel "${ffmpeg_loglevel}" \
        -f mpegts \
        -i pipe:0 \
        -c copy \
        -f mpegts \
        "${srt_url}"
