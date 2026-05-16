#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage: compare_viterbi_decoders.sh OLD_BIN NEW_BIN --input IQ_FILE -- ARGS...

Runs two rbdvbt_rx binaries with the same input capture and arguments, captures
stdout TS bytes, and compares output size plus receiver counters from stderr.

Example:
  tools/compare_viterbi_decoders.sh ./old/rbdvbt_rx ./build/rbdvbt_rx \
    --input capture.s16 -- --stdin --input-format s16 --sample-rate 2000000 \
    --sr 333k --fec 2/3 --live --probe-constellation \
    --resample-to-dvbt-rate --dvbt-ir 1 --ts-out -
EOF
}

if [ "$#" -lt 5 ]; then
    usage
    exit 2
fi

old_bin=$1
new_bin=$2
shift 2

input_file=
if [ "${1:-}" = "--input" ] && [ "$#" -ge 3 ]; then
    input_file=$2
    shift 2
fi
if [ "${1:-}" != "--" ] || [ -z "$input_file" ]; then
    usage
    exit 2
fi
shift

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/rbdvbt-viterbi-compare.XXXXXX")
cleanup() {
    rm -rf "$tmpdir"
}
trap cleanup EXIT

run_one() {
    local label=$1
    local bin=$2
    local out_ts="$tmpdir/$label.ts"
    local err_log="$tmpdir/$label.log"
    local time_file="$tmpdir/$label.time"
    shift 2

    /usr/bin/time -f "elapsed_s=%e" -o "$time_file" \
        "$bin" "$@" < "$input_file" > "$out_ts" 2> "$err_log"
}

summarize_one() {
    local label=$1
    local out_ts="$tmpdir/$label.ts"
    local err_log="$tmpdir/$label.log"
    local time_file="$tmpdir/$label.time"

    awk -v label="$label" -v bytes="$(wc -c < "$out_ts")" '
        function field(name,    raw) {
            if (match($0, name "=[^ ]+")) {
                raw = substr($0, RSTART + length(name) + 1, RLENGTH - length(name) - 1)
                gsub(/[^0-9.]/, "", raw)
                return raw
            }
            return ""
        }
        /\[outer\]/ {
            v = field("written_packets"); if (v != "") packets += v + 0
            v = field("rs_bad"); if (v != "") rs_bad += v + 0
            v = field("rs_corrected"); if (v != "") rs_corrected += v + 0
            v = field("rs_uncorrectable"); if (v != "") rs_uncorrectable += v + 0
        }
        /cc_errors=/ {
            v = field("cc_errors"); if (v != "") cc_errors += v + 0
        }
        function note_viterbi(symbols, seconds) {
            blocks++
            total += seconds
            if (symbols == 64) {
                blocks64++
                total64 += seconds
                pair64_sum += seconds
                pair64_count++
                if (pair64_count == 2) {
                    blocks128pair++
                    total128pair += pair64_sum
                    pair64_sum = 0
                    pair64_count = 0
                }
            } else if (symbols == 128) {
                blocks128++
                total128 += seconds
            }
        }
        /\[viterbi-detail\]/ {
            v = field("symbols"); if (v != "") {
                symbols = v + 0
            } else {
                symbols = 0
            }
            dep = field("depuncture") + 0
            acs = field("acs") + 0
            tb = field("traceback") + 0
            pack = field("pack") + 0
            seconds = dep + acs + tb + pack
            if (seconds > 0) {
                note_viterbi(symbols, seconds)
            }
        }
        /\[viterbi-time\]/ {
            v = field("symbols"); if (v != "") {
                symbols = v + 0
            } else {
                symbols = 0
            }
            v = field("viterbi"); if (v != "") {
                note_viterbi(symbols, v + 0)
            }
        }
        END {
            printf "%s output_bytes=%u ts_packets_by_bytes=%u outer_packets=%u rs_bad=%u rs_corrected=%u rs_uncorrectable=%u cc_errors=%u viterbi_blocks=%u avg_block_s=%.6f avg_64_s=%.6f avg_128_s=%.6f avg_two_64_s=%.6f ",
                   label, bytes, int(bytes / 188), packets, rs_bad, rs_corrected,
                   rs_uncorrectable, cc_errors, blocks, blocks ? total / blocks : 0,
                   blocks64 ? total64 / blocks64 : 0, blocks128 ? total128 / blocks128 : 0,
                   blocks128pair ? total128pair / blocks128pair : 0
        }
    ' "$err_log"
    cat "$time_file"
}

run_one old "$old_bin" "$@"
run_one new "$new_bin" "$@"

summarize_one old
summarize_one new

if cmp -s "$tmpdir/old.ts" "$tmpdir/new.ts"; then
    echo "ts_stdout=identical"
else
    echo "ts_stdout=different"
    exit 1
fi
