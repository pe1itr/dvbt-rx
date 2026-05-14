#!/usr/bin/env bash
set -euo pipefail

build_dir="${BUILD_DIR:-build}"
airscatter_dir="${AIRSCATTER_DIR:-/home/rhardenb/repo-prop/airplanescatter}"
work_dir="${WORK_DIR:-/tmp/rbdvbt-airscatter-loop}"
sr_list="${SR_LIST:-${SR:-150k 250k 333k}}"
snr_list="${SNR_LIST:-3 4 5 6 7 8 9}"
distance_list="${DISTANCE_LIST:-200 300 400}"
crossing_list="${CROSSING_LIST:-30 70}"
path_ratio_offset_db="${PATH_RATIO_OFFSET_DB:--10}"
packets="${PACKETS:-100000}"
max_samples="${MAX_SAMPLES:-0}"
ts_path="${TS_PATH:-}"
keep_impaired_iq="${KEEP_IMPAIRED_IQ:-0}"
fec="${FEC:-1/2}"
gi="${GI:-1/32}"
test_duration_s="${TEST_DURATION_S:-40}"
scenario_span_s="${SCENARIO_SPAN_S:-${test_duration_s}}"
scenario_step_s="${SCENARIO_STEP_S:-0.1}"

if [ "${fec}" = "auto" ]; then
    printf "FEC=auto is not supported for airscatter looptests; set FEC to 1/2, 2/3, 3/4, 5/6, or 7/8.\n" >&2
    exit 2
fi
if [ "${gi}" = "auto" ]; then
    printf "GI=auto is not supported for airscatter looptests; set GI to 1/32, 1/16, 1/8, or 1/4.\n" >&2
    exit 2
fi

mkdir -p "${work_dir}/scenarios" "${work_dir}/iq" "${work_dir}/ts" "${work_dir}/logs"

metrics_header="live_chunks,live_ok_chunks,live_failed_chunks,demod_reports,pilot_lock_min,pilot_lock_avg,pilot_lock_max,snr_min_db,snr_avg_db,snr_max_db,frontend_cont_bad,outer_acquired,outer_degraded,outer_relocks,outer_acquire_failed,outer_reports,outer_processed_blocks,outer_written_packets,outer_rs_bad,outer_rs_corrected,outer_rs_corrected_bytes,outer_rs_uncorrectable,ts_packets,ts_sync_bad,ts_transport_errors,ts_cc_errors,ts_pat_packets,ts_pmt_packets,ts_sdt_packets,health_reports,health_lock_min,health_lock_avg,health_snr_min_db,health_snr_avg_db,health_cont_bad,health_fifo_max,health_fifo_drops,health_fifo_drop_symbols,health_low_pilot,health_packets,health_rs_uncorr,health_cc,health_tei,health_sync_bad"

zero_metrics="0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"

extract_rx_metrics() {
    log_path="$1"
    if [ ! -f "${log_path}" ]; then
        printf "%s\n" "${zero_metrics}"
        return
    fi

    awk '
    function field(name, raw, pat) {
        pat = name "=[^ ]+"
        if (match($0, pat)) {
            raw = substr($0, RSTART + length(name) + 1, RLENGTH - length(name) - 1)
            gsub(/[,;]/, "", raw)
            gsub(/dB/, "", raw)
            return raw
        }
        return ""
    }
    function add_minmax(v, prefix) {
        v += 0
        if (prefix == "pilot") {
            pilot_count++
            pilot_sum += v
            if (!pilot_have || v < pilot_min) pilot_min = v
            if (!pilot_have || v > pilot_max) pilot_max = v
            pilot_have = 1
        } else if (prefix == "snr") {
            snr_count++
            snr_sum += v
            if (!snr_have || v < snr_min) snr_min = v
            if (!snr_have || v > snr_max) snr_max = v
            snr_have = 1
        }
    }
    /\[live\] eof/ {
        v = field("chunks"); if (v != "") live_chunks = v + 0
        v = field("ok"); if (v != "") live_ok_chunks = v + 0
        v = field("failed"); if (v != "") live_failed_chunks = v + 0
    }
    /\[dvbt2k\]/ {
        demod_reports++
        v = field("avg_pilot_lock"); if (v != "") add_minmax(v, "pilot")
        v = field("snr"); if (v != "") add_minmax(v, "snr")
    }
    /\[frontend-cont\]/ {
        v = field("continuous")
        if (v != "" && (v + 0) == 0) frontend_cont_bad++
    }
    /\[outer-state\]/ {
        if (index($0, "acquired grdvbt_stream")) outer_acquired++
        if (index($0, "degraded grdvbt_stream")) outer_degraded++
        if (index($0, "relock grdvbt_stream")) outer_relocks++
        if (index($0, "grdvbt acquire") && index($0, "failed")) outer_acquire_failed++
    }
    /\[outer\] grdvbt_stream/ {
        outer_reports++
        v = field("processed_blocks"); if (v != "") outer_processed_blocks += v + 0
        v = field("written_packets"); if (v != "") outer_written_packets += v + 0
        v = field("rs_bad"); if (v != "") outer_rs_bad += v + 0
        v = field("rs_corrected"); if (v != "") outer_rs_corrected += v + 0
        v = field("rs_corrected_bytes"); if (v != "") outer_rs_corrected_bytes += v + 0
        v = field("rs_uncorrectable"); if (v != "") outer_rs_uncorrectable += v + 0
    }
    /\[ts\] packets=/ {
        v = field("packets"); if (v != "") ts_packets += v + 0
        v = field("sync_bad"); if (v != "") ts_sync_bad += v + 0
        v = field("transport_errors"); if (v != "") ts_transport_errors += v + 0
        v = field("cc_errors"); if (v != "") ts_cc_errors += v + 0
        v = field("pat_packets"); if (v != "") ts_pat_packets += v + 0
        v = field("pmt_packets"); if (v != "") ts_pmt_packets += v + 0
        v = field("sdt_packets"); if (v != "") ts_sdt_packets += v + 0
    }
    /\[health\]/ {
        health_reports++
        v = field("lock_min")
        if (v != "") {
            v += 0
            if (!health_lock_have || v < health_lock_min) health_lock_min = v
            health_lock_have = 1
        }
        v = field("lock_avg"); if (v != "") health_lock_sum += v + 0
        v = field("snr_min")
        if (v != "") {
            v += 0
            if (!health_snr_have || v < health_snr_min) health_snr_min = v
            health_snr_have = 1
        }
        v = field("snr_avg"); if (v != "") health_snr_sum += v + 0
        v = field("cont_bad"); if (v != "") health_cont_bad += v + 0
        v = field("fifo_max"); if (v != "" && (v + 0) > health_fifo_max) health_fifo_max = v + 0
        v = field("fifo_drops"); if (v != "") health_fifo_drops += v + 0
        v = field("fifo_drop_symbols"); if (v != "") health_fifo_drop_symbols += v + 0
        v = field("low_pilot"); if (v != "") health_low_pilot += v + 0
        v = field("packets"); if (v != "") health_packets += v + 0
        v = field("rs_uncorr"); if (v != "") health_rs_uncorr += v + 0
        v = field("cc"); if (v != "") health_cc += v + 0
        v = field("tei"); if (v != "") health_tei += v + 0
        v = field("sync_bad"); if (v != "") health_sync_bad += v + 0
    }
    END {
        pilot_avg = pilot_count ? pilot_sum / pilot_count : 0
        snr_avg = snr_count ? snr_sum / snr_count : 0
        health_lock_avg = health_reports ? health_lock_sum / health_reports : 0
        health_snr_avg = health_reports ? health_snr_sum / health_reports : 0
        printf "%d,%d,%d,%d,%.5f,%.5f,%.5f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.5f,%.5f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", \
            live_chunks + 0, live_ok_chunks + 0, live_failed_chunks + 0, demod_reports + 0, \
            pilot_min + 0, pilot_avg, pilot_max + 0, snr_min + 0, snr_avg, snr_max + 0, frontend_cont_bad + 0, \
            outer_acquired + 0, outer_degraded + 0, outer_relocks + 0, outer_acquire_failed + 0, outer_reports + 0, \
            outer_processed_blocks + 0, outer_written_packets + 0, outer_rs_bad + 0, outer_rs_corrected + 0, \
            outer_rs_corrected_bytes + 0, outer_rs_uncorrectable + 0, ts_packets + 0, ts_sync_bad + 0, \
            ts_transport_errors + 0, ts_cc_errors + 0, ts_pat_packets + 0, ts_pmt_packets + 0, ts_sdt_packets + 0, \
            health_reports + 0, health_lock_min + 0, health_lock_avg, health_snr_min + 0, health_snr_avg, \
            health_cont_bad + 0, health_fifo_max + 0, health_fifo_drops + 0, health_fifo_drop_symbols + 0, \
            health_low_pilot + 0, health_packets + 0, health_rs_uncorr + 0, health_cc + 0, health_tei + 0, health_sync_bad + 0
    }
    ' "${log_path}"
}

if [ ! -x "${build_dir}/portsdown_iq_dump" ] ||
   [ ! -x "${build_dir}/iq_airscatter_channel" ] ||
   [ ! -x "${build_dir}/rbdvbt_rx" ]; then
    cmake -S . -B "${build_dir}"
    cmake --build "${build_dir}" -j
fi

summary="${work_dir}/summary.csv"
printf "symbol_rate,sample_rate_hz,target_snr_db,distance_km,crossing_deg,scenario_csv,impaired_iq,ts_out,status_json,rx_exit,ts_bytes,%s\n" "${metrics_header}" > "${summary}"

for symbol_rate in ${sr_list}; do
    case "${symbol_rate}" in
        150k) bandwidth=150000 ;;
        250k) bandwidth=250000 ;;
        333k) bandwidth=333000 ;;
        *) bandwidth="${symbol_rate}" ;;
    esac
    sample_rate=$((bandwidth * 8 / 7))
    desired_samples=$((sample_rate * test_duration_s))
    clean_samples="${max_samples}"
    if [ "${clean_samples}" = "0" ]; then
        clean_samples="${desired_samples}"
    fi

    clean_iq="${work_dir}/iq/clean_${symbol_rate}_${fec//\//}_${clean_samples}samp.iq"
    if [ ! -f "${clean_iq}" ]; then
        portsdown_args=(
            "--out" "${clean_iq}"
            "--bandwidth" "${bandwidth}"
            "--fec" "${fec}"
            "--gi" "${gi}"
            "--packets" "${packets}"
            "--max-samples" "${clean_samples}"
        )
        if [ -n "${ts_path}" ]; then
            portsdown_args+=("--ts" "${ts_path}")
        else
            portsdown_args+=("--null")
        fi
        "${build_dir}/portsdown_iq_dump" "${portsdown_args[@]}"
    fi

    for distance_km in ${distance_list}; do
        for crossing_deg in ${crossing_list}; do
            base="b737_scatter_432MHz_${distance_km}km_${crossing_deg}deg_dvbt_sr${symbol_rate}"
            scenario_csv="${work_dir}/scenarios/${base}_dvbt_metadata.csv"

            if [ ! -f "${scenario_csv}" ]; then
                (
                    cd "${airscatter_dir}"
                    PYTHONPATH="${airscatter_dir}/pysim" python3 "${airscatter_dir}/pysim/b737_scatter_plot.py" \
                        --distance-km "${distance_km}" \
                        --crossing-angle-deg "${crossing_deg}" \
                        --frequency-mhz 432 \
                        --span-s "${scenario_span_s}" \
                        --step-s "${scenario_step_s}" \
                        --dvbt-mode "${symbol_rate}" \
                        --dvbt-guard-interval "${gi}" \
                        --dvbt-noise-dbm -130 \
                        --dvbt-metadata-csv "${scenario_csv}" \
                        --output "${work_dir}/scenarios/${base}.png" \
                        --waterfall-output "${work_dir}/scenarios/${base}_waterfall.png" \
                        --no-waterfall
                )
            fi

            for target_snr_db in ${snr_list}; do
                run_base="${base}_snr${target_snr_db}"
                impaired_iq="${work_dir}/iq/${run_base}.iq"
                ts_out="${work_dir}/ts/${run_base}.ts"
                status_json="${work_dir}/logs/${run_base}.json"
                rx_log="${work_dir}/logs/${run_base}.log"
                channel_report="${work_dir}/logs/${run_base}_channel.csv"

                "${build_dir}/iq_airscatter_channel" \
                    --in "${clean_iq}" \
                    --out "${impaired_iq}" \
                    --scenario "${scenario_csv}" \
                    --sample-rate "${sample_rate}" \
                    --target-snr-db "${target_snr_db}" \
                    --path-ratio-offset-db "${path_ratio_offset_db}" \
                    --report "${channel_report}"

                set +e
                "${build_dir}/rbdvbt_rx" \
                    --probe-constellation \
                    --resample-to-dvbt-rate \
                    --dvbt-ir 1 \
                    --stdin \
                    --live \
                    --input-format s16 \
                    --sample-rate "${sample_rate}" \
                    --sr "${symbol_rate}" \
                    --gi "${gi}" \
                    --fec "${fec}" \
                    --ts-out "${ts_out}" \
                    --status-json "${status_json}" \
                    --loglevel info \
                    < "${impaired_iq}" > /dev/null 2> "${rx_log}"
                rx_exit=$?
                set -e

                ts_bytes=0
                if [ -f "${ts_out}" ]; then
                    ts_bytes=$(wc -c < "${ts_out}")
                fi
                metrics=$(extract_rx_metrics "${rx_log}")
                printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
                    "${symbol_rate}" "${sample_rate}" "${target_snr_db}" \
                    "${distance_km}" "${crossing_deg}" "${scenario_csv}" "${impaired_iq}" \
                    "${ts_out}" "${status_json}" "${rx_exit}" "${ts_bytes}" "${metrics}" >> "${summary}"
                if [ "${keep_impaired_iq}" != "1" ]; then
                    rm -f "${impaired_iq}"
                fi
            done
        done
    done
done

printf "Airscatter looptest summary: %s\n" "${summary}"
