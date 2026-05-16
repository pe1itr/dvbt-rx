#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_dir="$(CDPATH= cd -- "${script_dir}/.." && pwd)"

build_dir="${BUILD_DIR:-${repo_dir}/build}"
rbdvbt_rx="${RBDVBT_RX:-${build_dir}/rbdvbt_rx}"
rtl_sdr_bin="${RTL_SDR:-rtl_sdr}"
ffmpeg_bin="${FFMPEG:-${repo_dir}/../ffmpeg/ffmpeg}"

frequency="${FREQUENCY:-436000000}"
device="${RTL_DEVICE:-00000001}"
sample_rate="${SAMPLERATE:-1010526}"
symbol_rate="${SYMBOLRATE:-333000}"
gain="${GAIN:-30}"
gi="${GI:-1/32}"
fec="${FEC:-2/3}"
live_symbols="${LIVE_SYMBOLS:-128}"
probe_symbols="${PROBE_SYMBOLS:-64}"
afc="${AFC:-1}"
input_format="${INPUT_FORMAT:-u8}"
status_json="${STATUS_JSON:-/var/www/html/dvb/dvbt-rx-status.json}"
loglevel="${LOGLEVEL:-quiet}"
session="${SESSION:-$(date +%Y%m%d_%H%M%S)}"
log_dir="${LOG_DIR:-${repo_dir}/logs}"
log_keep="${LOG_KEEP:-10}"
rxlog="${RXLOG:-${log_dir}/rx_${session}.log}"
ffmpeglog="${FFMPEGLOG:-}"
ffmpeglog_help="${FFMPEGLOG:-temporary run log}"
srt_url="${SRT_URL:-srt://44.137.26.85:4001?mode=caller&latency=500000}"
ffmpeg_loglevel="${FFMPEG_LOGLEVEL:-error}"
ffmpeg_probesize="${FFMPEG_PROBESIZE:-2000000}"
ffmpeg_analyzeduration="${FFMPEG_ANALYZEDURATION:-2000000}"
ffmpeg_map="${FFMPEG_MAP:-0:v:0}"
video_start="${WAIT_VIDEO_START:-1}"
enable_gui="${GUI:-0}"
udp_ts="${UDP_TS:-${UDP_OUT:-127.0.0.1:10000}}"
watchdog_enabled="${WATCHDOG_ENABLED:-1}"
lock_loss_timeout="${LOCK_LOSS_TIMEOUT:-15}"
status_stale_timeout="${STATUS_STALE_TIMEOUT:-10}"
ffmpeg_no_frame_limit="${FFMPEG_NO_FRAME_LIMIT:-20}"
watchdog_interval="${WATCHDOG_INTERVAL:-1}"
restart_on_watchdog="${RESTART_ON_WATCHDOG:-1}"
restart_delay="${RESTART_DELAY:-2}"
max_restarts="${MAX_RESTARTS:-0}"

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
  FFMPEG          ffmpeg binary path. Default: ${ffmpeg_bin}
  RTL_SDR         rtl_sdr binary path. Default: rtl_sdr
  RBDVBT_RX       receiver path. Default: ${build_dir}/rbdvbt_rx
  BUILD_DIR       build directory. Default: ${build_dir}
  STATUS_JSON     receiver status JSON. Default: ${status_json}
  LOGLEVEL        receiver loglevel. Default: ${loglevel}
  AFC             enable receiver AFC when set to 1. Default: ${afc}
  LOG_DIR         log directory. Default: ${log_dir}
  LOG_KEEP        receiver logs to keep in LOG_DIR. Default: ${log_keep}
  RXLOG           receiver log path. Default: ${rxlog}
  FFMPEGLOG       ffmpeg log path. Default: ${ffmpeglog_help}
  FFMPEG_LOGLEVEL ffmpeg loglevel. Default: ${ffmpeg_loglevel}
  FFMPEG_MAP      ffmpeg stream map for SRT output. Default: ${ffmpeg_map}
  WATCHDOG_ENABLED monitor and stop/restart pipeline when set to 1. Default: ${watchdog_enabled}
  LOCK_LOSS_TIMEOUT seconds without lock after first lock before watchdog action. Default: ${lock_loss_timeout}
  STATUS_STALE_TIMEOUT seconds without fresh JSON after first lock before watchdog action. Default: ${status_stale_timeout}
  FFMPEG_NO_FRAME_LIMIT recent h264 parser errors before restart. Default: ${ffmpeg_no_frame_limit}
  RESTART_ON_WATCHDOG restart pipeline after watchdog stop when set to 1. Default: ${restart_on_watchdog}
  RESTART_DELAY seconds to wait before a watchdog restart. Default: ${restart_delay}
  MAX_RESTARTS maximum watchdog restarts, 0 means unlimited. Default: ${max_restarts}
  GUI             pass --gui to rbdvbt_rx when set to 1. Default: 0
  UDP_TS          receiver-to-ffmpeg UDP TS endpoint. Default: ${udp_ts}
  UDP_OUT         compatibility alias for UDP_TS
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
if ! "${ffmpeg_bin}" -hide_banner -protocols 2>/dev/null | awk '$1 == "srt" { found = 1 } END { exit found ? 0 : 1 }'; then
    printf "ffmpeg has no SRT protocol support: %s\n" "${ffmpeg_bin}" >&2
    printf "Install an ffmpeg build with libsrt support or set FFMPEG=/path/to/ffmpeg-with-srt.\n" >&2
    exit 1
fi

mkdir -p "${log_dir}"

prune_logs() {
    dir="$1"
    pattern="$2"
    keep="$3"

    if [ "${keep}" = "0" ]; then
        return
    fi
    find "${dir}" -maxdepth 1 -type f -name "${pattern}" -printf '%T@ %p\n' 2>/dev/null |
        sort -rn |
        awk -v keep="${keep}" 'NR > keep { sub(/^[^ ]+ /, ""); print }' |
        while IFS= read -r old_log; do
            rm -f -- "${old_log}"
        done
}

prune_logs "${log_dir}" "rx_*.log" "${log_keep}"

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
    --status-json "${status_json}"
    --loglevel "${loglevel}"
)

if [ "${video_start}" = "1" ]; then
    rx_args+=(--wait-video-start)
fi
if [ "${afc}" = "1" ]; then
    rx_args+=(--afc)
else
    rx_args+=(--no-afc)
fi
if [ "${enable_gui}" = "1" ]; then
    rx_args+=(--gui)
fi

rx_udp_ts=""
ffmpeg_ts_input=""
case "${udp_ts}" in
    ""|0|off|OFF|none|NONE) udp_ts="" ;;
esac
if [ -n "${udp_ts}" ]; then
    case "${udp_ts}" in
        udp://*) rx_udp_ts="${udp_ts#udp://}" ;;
        *) rx_udp_ts="${udp_ts}" ;;
    esac
    rx_udp_ts="${rx_udp_ts%%\?*}"
    udp_port="${rx_udp_ts##*:}"
    case "${udp_port}" in
        ""|*[!0-9]*)
            printf "Invalid UDP_TS endpoint, expected HOST:PORT: %s\n" "${udp_ts}" >&2
            exit 2
            ;;
    esac
    rx_args+=(--udp-ts "${rx_udp_ts}")
    ffmpeg_ts_input="udp://@:${udp_port}?fifo_size=1000000&overrun_nonfatal=1"
else
    rx_args+=(--stdout-ts)
fi

json_bool() {
    key="$1"
    file="$2"
    sed -n "s/.*\"${key}\"[[:space:]]*:[[:space:]]*\\(true\\|false\\).*/\\1/p" "${file}" 2>/dev/null | tail -n 1
}

json_uint() {
    key="$1"
    file="$2"
    sed -n "s/.*\"${key}\"[[:space:]]*:[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" "${file}" 2>/dev/null | tail -n 1
}

ffmpeg_h264_error_count() {
    file="$1"
    offset="$2"
    size=0

    if [ ! -s "${file}" ]; then
        printf "0 0\n"
        return 0
    fi
    size="$(wc -c < "${file}" 2>/dev/null | tr -d ' ' || printf "0")"
    if [ "${size}" -le "${offset}" ]; then
        printf "0 %s\n" "${size}"
        return 0
    fi
    tail -c "+$((offset + 1))" "${file}" 2>/dev/null | awk -v size="${size}" '
        /non-existing PPS/ { count++ }
        /no frame!/ { count++ }
        END { printf "%u %s\n", count, size }
    '
}

cleanup() {
    trap - INT TERM EXIT
    if [ -n "${rtl_pid:-}" ]; then
        kill "${rtl_pid}" 2>/dev/null || true
    fi
    if [ -n "${rx_pid:-}" ]; then
        kill "${rx_pid}" 2>/dev/null || true
    fi
    if [ -n "${ffmpeg_pid:-}" ]; then
        kill "${ffmpeg_pid}" 2>/dev/null || true
    fi
    if [ -n "${rtl_pid:-}" ]; then
        wait "${rtl_pid}" 2>/dev/null || true
    fi
    if [ -n "${rx_pid:-}" ]; then
        wait "${rx_pid}" 2>/dev/null || true
    fi
    if [ -n "${ffmpeg_pid:-}" ]; then
        wait "${ffmpeg_pid}" 2>/dev/null || true
    fi
    if [ -n "${run_dir:-}" ]; then
        rm -rf "${run_dir}"
    fi
    rtl_pid=""
    rx_pid=""
    ffmpeg_pid=""
    run_dir=""
}

request_watchdog_stop() {
    reason="$1"
    printf "Stopping receiver pipeline: %s\n" "${reason}" >&2
    watchdog_reason="${reason}"
    cleanup
    return 0
}

printf "Starting RTL-SDR receiver to SRT\n" >&2
printf "  RF:        %s Hz, sample rate %s, gain %s, device %s\n" "${frequency}" "${sample_rate}" "${gain}" "${device}" >&2
printf "  DVB-T:     SR %s, GI %s, FEC %s\n" "${symbol_rate}" "${gi}" "${fec}" >&2
printf "  SRT:       %s\n" "${srt_url}" >&2
if [ -n "${rx_udp_ts}" ]; then
    printf "  TS path:   receiver UDP %s -> ffmpeg %s\n" "${rx_udp_ts}" "${ffmpeg_ts_input}" >&2
else
    printf "  TS path:   receiver stdout -> ffmpeg FIFO\n" >&2
fi
printf "  RX log:    %s\n" "${rxlog}" >&2
printf "  FFmpeg log: %s\n" "${ffmpeglog_help}" >&2
printf "  Status:    %s\n" "${status_json}" >&2
printf "  Watchdog:  enabled=%s lock_loss=%ss stale_json=%ss h264_errors=%s restart=%s delay=%ss max=%s\n" \
    "${watchdog_enabled}" "${lock_loss_timeout}" "${status_stale_timeout}" "${ffmpeg_no_frame_limit}" \
    "${restart_on_watchdog}" "${restart_delay}" "${max_restarts}" >&2

: > "${rxlog}"
trap cleanup INT TERM EXIT

run_pipeline_once() {
    attempt="$1"
    pipeline_rc=0
    watchdog_reason=""
    trap cleanup INT TERM EXIT
    run_dir="$(mktemp -d "/tmp/dvbt-rx.${session}.XXXXXX")"
    current_ffmpeglog="${ffmpeglog}"
    if [ -z "${current_ffmpeglog}" ]; then
        current_ffmpeglog="${run_dir}/ffmpeg.log"
    fi

    printf "[pipeline] attempt=%s ffmpeg_log=%s\n" "${attempt}" "${current_ffmpeglog}" >&2
    printf "[pipeline] attempt=%s start\n" "${attempt}" >> "${rxlog}"

    iq_fifo="${run_dir}/iq.u8"
    mkfifo "${iq_fifo}"
    current_ffmpeg_ts_input="${ffmpeg_ts_input}"
    if [ -z "${current_ffmpeg_ts_input}" ]; then
        ts_fifo="${run_dir}/rx.ts"
        mkfifo "${ts_fifo}"
        current_ffmpeg_ts_input="${ts_fifo}"
    else
        ts_fifo=""
    fi

    ffmpeg_args=(
        -hide_banner
        -loglevel "${ffmpeg_loglevel}"
        -fflags nobuffer
        -f mpegts
        -probesize "${ffmpeg_probesize}"
        -analyzeduration "${ffmpeg_analyzeduration}"
        -i "${current_ffmpeg_ts_input}"
        -map "${ffmpeg_map}"
        -c copy
        -f mpegts
        "${srt_url}"
    )

    "${ffmpeg_bin}" "${ffmpeg_args[@]}" 2> "${current_ffmpeglog}" &
    ffmpeg_pid=$!

    "${rtl_sdr_bin}" -d "${device}" -f "${frequency}" -s "${sample_rate}" -g "${gain}" - > "${iq_fifo}" &
    rtl_pid=$!

    if [ -n "${rx_udp_ts}" ]; then
        "${rbdvbt_rx}" "${rx_args[@]}" < "${iq_fifo}" > /dev/null 2>> "${rxlog}" &
    else
        "${rbdvbt_rx}" "${rx_args[@]}" < "${iq_fifo}" > "${ts_fifo}" 2>> "${rxlog}" &
    fi
    rx_pid=$!

    seen_lock=0
    lock_lost_since=0
    ffmpeg_log_offset=0
    ffmpeg_h264_errors=0
    start_time="$(date +%s)"

    while kill -0 "${rtl_pid}" 2>/dev/null &&
          kill -0 "${rx_pid}" 2>/dev/null &&
          kill -0 "${ffmpeg_pid}" 2>/dev/null; do
        if [ "${watchdog_enabled}" = "1" ]; then
            now="$(date +%s)"
            locked=""
            frontend_locked=""
            updated=""
            if [ -f "${status_json}" ]; then
                locked="$(json_bool locked "${status_json}")"
                frontend_locked="$(json_bool lamp_ofdm_sync "${status_json}")"
                updated="$(json_uint updated_unix "${status_json}")"
            fi
            if [ -n "${updated}" ] && [ "${updated}" -lt "${start_time}" ]; then
                locked=""
                frontend_locked=""
                updated=""
            fi

            if [ "${locked}" = "true" ] || [ "${frontend_locked}" = "true" ]; then
                seen_lock=1
                lock_lost_since=0
            elif [ "${seen_lock}" = "1" ]; then
                if [ "${lock_lost_since}" = "0" ]; then
                    lock_lost_since="${now}"
                elif [ $((now - lock_lost_since)) -ge "${lock_loss_timeout}" ]; then
                    request_watchdog_stop "receiver lock lost for ${lock_loss_timeout}s"
                    return 75
                fi
            fi

            if [ "${seen_lock}" = "1" ]; then
                if [ -z "${updated}" ]; then
                    if [ $((now - start_time)) -ge "${status_stale_timeout}" ]; then
                        request_watchdog_stop "status JSON missing after receiver lock"
                        return 75
                    fi
                elif [ $((now - updated)) -ge "${status_stale_timeout}" ]; then
                    request_watchdog_stop "status JSON stale for ${status_stale_timeout}s"
                    return 75
                fi
            fi

            if [ "${ffmpeg_no_frame_limit}" != "0" ]; then
                read -r new_ffmpeg_h264_errors ffmpeg_log_offset <<EOF
$(ffmpeg_h264_error_count "${current_ffmpeglog}" "${ffmpeg_log_offset}")
EOF
                if [ "${new_ffmpeg_h264_errors}" -gt 0 ]; then
                    ffmpeg_h264_errors=$((ffmpeg_h264_errors + new_ffmpeg_h264_errors))
                else
                    ffmpeg_h264_errors=0
                fi
                if [ "${ffmpeg_h264_errors}" -ge "${ffmpeg_no_frame_limit}" ]; then
                    request_watchdog_stop "ffmpeg h264 parser stuck after repeated no-frame/PPS errors"
                    return 75
                fi
            fi
        fi
        sleep "${watchdog_interval}"
    done

    wait "${rtl_pid}" || pipeline_rc=$?
    wait "${rx_pid}" || pipeline_rc=$?
    wait "${ffmpeg_pid}" || pipeline_rc=$?
    cleanup
    return "${pipeline_rc}"
}

attempt=1
restart_count=0
while true; do
    if run_pipeline_once "${attempt}"; then
        exit 0
    fi
    pipeline_rc=$?

    if [ "${pipeline_rc}" != "75" ] || [ "${restart_on_watchdog}" != "1" ]; then
        exit "${pipeline_rc}"
    fi
    if [ "${max_restarts}" != "0" ] && [ "${restart_count}" -ge "${max_restarts}" ]; then
        printf "Watchdog restart limit reached after %s restarts\n" "${restart_count}" >&2
        exit "${pipeline_rc}"
    fi

    restart_count=$((restart_count + 1))
    attempt=$((attempt + 1))
    printf "Restarting receiver pipeline after watchdog stop (%s), restart %s\n" \
        "${watchdog_reason:-unknown}" "${restart_count}" >&2
    sleep "${restart_delay}"
done
