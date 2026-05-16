#!/usr/bin/env python3
"""Replay PA3BYV IQ segments and collect receiver metrics.

This is intentionally small and self-contained so it can be used before and
after decoder changes to compare the same fading events.
"""

import argparse
import csv
import json
import os
import re
import subprocess
import sys
import tempfile
import time


SAMPLE_RATE = 1010526
BYTES_PER_COMPLEX_SAMPLE = 4


TARGETS = [
    {
        "name": "250k_main_fade",
        "recording": "recordings/linrad_20260516_222622_s16.iq",
        "sr": "250k",
        "live_symbols": 64,
        "chunk_samples": 478068,
        "offset_s": 165.0,
        "duration_s": 18.0,
        "notes": "Main 250k TS burst with low-pilot dip between TS moments.",
    },
    {
        "name": "150k_between_ts_fade",
        "recording": "recordings/linrad_20260516_224900_s16.iq",
        "sr": "150k",
        "live_symbols": 128,
        "chunk_samples": 1593559,
        "offset_s": 38.0,
        "duration_s": 24.0,
        "notes": "150k fade between two TS moments.",
    },
    {
        "name": "150k_pre_big_burst",
        "recording": "recordings/linrad_20260516_225314_s16.iq",
        "sr": "150k",
        "live_symbols": 128,
        "chunk_samples": 1593559,
        "offset_s": 121.0,
        "duration_s": 24.0,
        "notes": "Calibrated pre-burst low-pilot drop followed by the larger 150k TS burst.",
    },
    {
        "name": "150k_strong_then_fade",
        "recording": "recordings/linrad_20260516_220739_s16.iq",
        "sr": "150k",
        "live_symbols": 128,
        "chunk_samples": 1593559,
        "offset_s": 274.0,
        "duration_s": 23.0,
        "notes": "Calibrated strong 150k lock and TS followed by fading.",
    },
]


def sample_count(seconds):
    return int(round(seconds * SAMPLE_RATE))


def byte_offset(seconds):
    return sample_count(seconds) * BYTES_PER_COMPLEX_SAMPLE


def parse_metrics(stderr_text, elapsed_s):
    metrics = {
        "elapsed_s": elapsed_s,
        "ts_packets_total": 0,
        "ts_reports": 0,
        "ts_reports_with_packets": 0,
        "pat_packets_total": 0,
        "pmt_packets_total": 0,
        "sdt_packets_total": 0,
        "cc_errors_total": 0,
        "transport_errors_total": 0,
        "sync_bad_total": 0,
        "video_pid_reports": 0,
        "audio_pid_reports": 0,
        "low_pilot_drops": 0,
        "fifo_reset_low_pilot": 0,
        "fifo_reset_frontend_discontinuity": 0,
        "track_rejects": 0,
        "frontend_continuous_1": 0,
        "frontend_continuous_0": 0,
        "outer_relock": 0,
        "outer_relock_hard": 0,
        "outer_relock_soft": 0,
        "outer_degraded": 0,
        "outer_productive_degraded": 0,
        "outer_local_realign_attempts": 0,
        "outer_local_realign_acquired": 0,
        "outer_local_realign_failed": 0,
        "first_ts_report": -1,
        "max_snr_db": None,
        "avg_snr_db": None,
        "max_pilot_lock": None,
        "avg_pilot_lock": None,
    }
    snrs = []
    locks = []

    for line_no, line in enumerate(stderr_text.splitlines(), 1):
        if line.startswith("[dvbt2k]"):
            if "dropping weak live chunk" in line:
                metrics["low_pilot_drops"] += 1
            m = re.search(r"snr=([-0-9.]+)dB", line)
            if m:
                snrs.append(float(m.group(1)))
            m = re.search(r"avg_pilot_lock=([-0-9.]+)", line)
            if m:
                locks.append(float(m.group(1)))

        if line.startswith("[fifo1] reset"):
            if "reason=low-pilot" in line:
                metrics["fifo_reset_low_pilot"] += 1
            if "reason=frontend-discontinuity" in line:
                metrics["fifo_reset_frontend_discontinuity"] += 1

        if "grdvbt-track reject" in line:
            metrics["track_rejects"] += 1

        if line.startswith("[frontend-cont]"):
            if "continuous=1" in line:
                metrics["frontend_continuous_1"] += 1
            if "continuous=0" in line:
                metrics["frontend_continuous_0"] += 1

        if line.startswith("[outer-state] degraded"):
            metrics["outer_degraded"] += 1
            if "productive=1" in line:
                metrics["outer_productive_degraded"] += 1

        if line.startswith("[outer-state] local_realign"):
            if "attempt" in line:
                metrics["outer_local_realign_attempts"] += 1
            if "acquired" in line:
                metrics["outer_local_realign_acquired"] += 1
            if "failed" in line:
                metrics["outer_local_realign_failed"] += 1

        if line.startswith("[outer-state] relock"):
            metrics["outer_relock"] += 1
            if "hard_cadence_fail" in line:
                metrics["outer_relock_hard"] += 1
            if "consecutive_fail_jobs" in line:
                metrics["outer_relock_soft"] += 1

        if line.startswith("[ts]"):
            metrics["ts_reports"] += 1
            packets = int_field(line, "packets")
            metrics["ts_packets_total"] += packets
            metrics["pat_packets_total"] += int_field(line, "pat_packets")
            metrics["pmt_packets_total"] += int_field(line, "pmt_packets")
            metrics["sdt_packets_total"] += int_field(line, "sdt_packets")
            metrics["cc_errors_total"] += int_field(line, "cc_errors")
            metrics["transport_errors_total"] += int_field(line, "transport_errors")
            metrics["sync_bad_total"] += int_field(line, "sync_bad")
            if packets > 0:
                metrics["ts_reports_with_packets"] += 1
                if metrics["first_ts_report"] < 0:
                    metrics["first_ts_report"] = line_no
            if re.search(r"video_pid=0x[0-9a-fA-F]+", line):
                metrics["video_pid_reports"] += 1
            if re.search(r"audio_pid=0x[0-9a-fA-F]+", line):
                metrics["audio_pid_reports"] += 1

    if snrs:
        metrics["max_snr_db"] = max(snrs)
        metrics["avg_snr_db"] = sum(snrs) / len(snrs)
    if locks:
        metrics["max_pilot_lock"] = max(locks)
        metrics["avg_pilot_lock"] = sum(locks) / len(locks)

    return metrics


def int_field(line, name):
    m = re.search(r"\b" + re.escape(name) + r"=(\d+)", line)
    return int(m.group(1)) if m else 0


def run_target(target, args):
    target = dict(target)
    if args.offset is not None:
        target["offset_s"] = args.offset
    if args.duration is not None:
        target["duration_s"] = args.duration
    effective_offset_s = target["offset_s"] + args.offset_shift
    skip = byte_offset(effective_offset_s)
    count = sample_count(target["duration_s"]) * BYTES_PER_COMPLEX_SAMPLE
    cmd = [
        args.rx,
        "--stdin",
        "--input-format",
        "s16",
        "--sample-rate",
        str(SAMPLE_RATE),
        "--resample-to-dvbt-rate",
        "--dvbt-ir",
        "1",
        "--sr",
        target["sr"],
        "--gi",
        "1/32",
        "--fec",
        args.fec,
        "--live",
        "--live-symbols",
        str(target["live_symbols"]),
        "--probe-symbols",
        str(target["live_symbols"]),
        "--loglevel",
        args.loglevel,
    ]
    if args.max_samples:
        cmd.extend(["--max-samples", str(args.max_samples)])
    if args.afc:
        cmd.append("--afc")
    else:
        cmd.append("--no-afc")
    if args.probe_constellation:
        cmd.append("--probe-constellation")
    remove_ts_after = False
    if args.ts_out:
        if getattr(args, "target_count", 1) > 1:
            root, ext = os.path.splitext(args.ts_out)
            ts_path = f"{root}_{target['name']}{ext or '.ts'}"
        else:
            ts_path = args.ts_out
    else:
        fd, ts_path = tempfile.mkstemp(prefix=f"pa3byv_{target['name']}_", suffix=".ts")
        os.close(fd)
        remove_ts_after = True
    cmd.extend(["--ts-out", ts_path])

    start = time.monotonic()
    with open(target["recording"], "rb") as iq:
        iq.seek(skip)
        segment = iq.read(count)
    proc = subprocess.run(
        cmd,
        input=segment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        check=False,
    )
    elapsed = time.monotonic() - start
    stderr_text = proc.stderr.decode("utf-8", errors="replace")
    metrics = parse_metrics(stderr_text, elapsed)
    ts_file_bytes = os.path.getsize(ts_path) if os.path.exists(ts_path) else 0
    metrics["ts_file"] = "" if remove_ts_after else ts_path
    metrics["ts_file_bytes"] = ts_file_bytes
    metrics["ts_file_packets"] = ts_file_bytes // 188
    metrics["ts_file_trailing_bytes"] = ts_file_bytes % 188
    if remove_ts_after:
        try:
            os.unlink(ts_path)
        except OSError:
            pass
    metrics.update(
        {
            "name": target["name"],
            "recording": target["recording"],
            "sr": target["sr"],
            "offset_s": target["offset_s"],
            "effective_offset_s": effective_offset_s,
            "duration_s": target["duration_s"],
            "returncode": proc.returncode,
            "notes": target["notes"],
        }
    )
    if args.keep_logs:
        log_path = os.path.join(args.keep_logs, target["name"] + ".log")
        os.makedirs(args.keep_logs, exist_ok=True)
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(stderr_text)
        metrics["log_path"] = log_path
    return metrics


def selected_targets(names):
    if names == ["all"]:
        return TARGETS
    wanted = set(names)
    return [t for t in TARGETS if t["name"] in wanted]


def print_table(rows):
    headers = [
        "name",
        "sr",
        "ts_packets_total",
        "ts_file_packets",
        "ts_reports_with_packets",
        "pat_packets_total",
        "pmt_packets_total",
        "cc_errors_total",
        "low_pilot_drops",
        "fifo_reset_low_pilot",
        "outer_degraded",
        "outer_productive_degraded",
        "outer_local_realign_attempts",
        "outer_local_realign_acquired",
        "outer_relock",
        "max_snr_db",
    ]
    print(",".join(headers))
    for row in rows:
        print(",".join(format_value(row.get(h)) for h in headers))


def format_value(value):
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def write_csv(path, rows):
    keys = sorted({k for row in rows for k in row.keys()})
    with open(path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rx", default="build/rbdvbt_rx", help="path to rbdvbt_rx")
    parser.add_argument("--target", action="append", default=["all"],
                        help="target name to run, or all")
    parser.add_argument("--list", action="store_true", help="list replay targets")
    parser.add_argument("--fec", default="1/2", help="FEC setting for replay")
    parser.add_argument("--loglevel", default="info")
    parser.add_argument("--max-samples", type=int, default=0,
                        help="optional receiver --max-samples override")
    parser.add_argument("--no-afc", dest="afc", action="store_false")
    parser.set_defaults(afc=True)
    parser.add_argument("--offset-shift", type=float, default=0.0,
                        help="seconds to add to every target offset")
    parser.add_argument("--offset", type=float, default=None,
                        help="override target offset in seconds; only use with one target")
    parser.add_argument("--duration", type=float, default=None,
                        help="override target duration in seconds")
    parser.add_argument("--no-probe-constellation", dest="probe_constellation",
                        action="store_false",
                        help="do not pass --probe-constellation to the receiver")
    parser.set_defaults(probe_constellation=True)
    parser.add_argument("--ts-out", default="", help="optional TS output path")
    parser.add_argument("--json-out", default="", help="write metrics JSON")
    parser.add_argument("--csv-out", default="", help="write metrics CSV")
    parser.add_argument("--keep-logs", default="", help="directory for stderr logs")
    args = parser.parse_args(argv)

    targets = selected_targets(args.target)
    if args.list:
        for t in TARGETS:
            print(f"{t['name']} {t['sr']} {t['recording']} "
                  f"offset={t['offset_s']} duration={t['duration_s']} {t['notes']}")
        return 0
    if not targets:
        print("no matching targets", file=sys.stderr)
        return 2

    rows = []
    args.target_count = len(targets)
    for target in targets:
        if not os.path.exists(target["recording"]):
            print(f"missing recording: {target['recording']}", file=sys.stderr)
            return 2
        rows.append(run_target(target, args))

    print_table(rows)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=2, sort_keys=True)
    if args.csv_out:
        write_csv(args.csv_out, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
