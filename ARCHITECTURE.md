# Architecture

## Overview

The receiver is a streaming pipeline:

```text
stdin IQ (cs16 @ 1010526 Hz)
  -> input parsing
  -> normalization / DC removal
  -> coarse frequency estimation
  -> resampling to internal RB-DVB-T rate
  -> guard interval correlation / symbol timing
  -> OFDM framing
  -> FFT
  -> pilot extraction
  -> fine CFO / common phase correction
  -> channel estimation
  -> equalization
  -> QPSK demapping
  -> deinterleaving
  -> Viterbi decoding
  -> Reed-Solomon decoding
  -> MPEG-TS packet recovery
  -> stdout
```

All human-readable diagnostics are sent to `stderr`.

## Architectural principles

### 1. Transport stream purity

`stdout` must contain TS data only.

### 2. Deterministic debugability

Every stage must expose numeric and textual health indicators.

### 3. Narrow first implementation

Only fixed reduced-bandwidth QPSK modes are supported initially.

### 4. Modular DSP pipeline

Each stage should be testable in isolation from recorded IQ files.

## Major modules

## 1. CLI and configuration

Responsible for:

- parsing arguments
- validating combinations
- building an immutable runtime configuration

Example config structure:

```text
sample_rate_in = 1010526
mode_sr        = 333k
guard_interval = 1/8
fec            = 1/2
modulation     = qpsk
stdin_input    = true
debug_flags    = [sync, pilots, eq, fec, ts]
```

## 2. Input source

Responsible for:

- reading interleaved `int16` IQ from `stdin`
- converting to complex float
- keeping block alignment stable
- counting underruns / short reads / EOF

Diagnostics:

- bytes read
- sample count
- min/max I/Q
- clipping counter
- DC estimate

## 3. Front-end conditioning

Functions:

- DC blocker
- optional amplitude normalization
- optional coarse AGC-lite

Diagnostics:

- DC estimate before and after correction
- average power
- peak power
- clipping and saturation warnings

## 4. Resampler

The physical input sample rate is fixed at `1010526 Hz`, while the DVB-T receiver core should operate at an internal rate matched to the selected RB-DVB-T preset.

Supported user presets:

- `125k`
- `150k`
- `250k`
- `333k`
- `500k`

The exact mapping from user-facing preset to internal OFDM timing constants is part of the receiver mode table.

Diagnostics:

- configured resampling ratio
- phase accumulator state
- fractional timing drift
- alias/overflow warnings

## 5. Acquisition and timing sync

This block uses guard interval correlation to estimate:

- symbol boundary
- coarse frequency offset
- timing quality metric

Outputs:

- estimated symbol start
- lock quality
- coarse CFO in Hz

Diagnostics:

- GI correlation peak
- peak-to-average ratio
- timing jitter
- lock / unlock state transitions

## 6. OFDM framer

Responsibilities:

- cut symbols from the sample stream
- strip guard interval
- deliver `N` useful samples to FFT
- maintain symbol numbering

Diagnostics:

- symbol count
- dropped symbols
- tracking adjustment

## 7. FFT engine

Responsibilities:

- run FFT on each OFDM symbol
- output complex frequency bins

Dependencies:

- likely FFTW3 or kissfft

Diagnostics:

- symbol energy
- DC bin magnitude
- bin occupancy statistics

## 8. Carrier map and pilot extraction

Responsibilities:

- identify data carriers
- identify continual/scattered pilots
- identify TPS carriers

For v1, the code may start with fixed known RB-DVB-T assumptions, even if TPS handling is incomplete.

Diagnostics:

- pilot energy
- pilot phase consistency
- pilot-to-data ratio

## 9. Fine frequency / phase tracker

Responsibilities:

- residual CFO correction
- common phase error correction

Diagnostics:

- fine CFO estimate
- per-symbol phase error
- drift trend

## 10. Channel estimator and equalizer

Responsibilities:

- estimate complex channel response from pilots
- interpolate across data carriers
- apply 1-tap equalization

Diagnostics:

- channel amplitude range
- channel phase slope
- equalizer variance
- post-equalization EVM-like metric

## 11. QPSK demapper

Responsibilities:

- map equalized carriers to soft or hard bit metrics
- produce symbol statistics

For v1, only QPSK is needed.

Diagnostics:

- I/Q cluster means
- decision margins
- constellation spread

## 12. Deinterleaver

Responsibilities:

- symbol deinterleaving
- bit deinterleaving

Diagnostics:

- frame counters
- table consistency checks
- bitflow sanity counters

## 13. Viterbi decoder

Responsibilities:

- convolutional decoding
- puncturing support for:
  - `1/2`
  - `2/3`
  - `3/4`

Diagnostics:

- branch metric summaries
- traceback status
- estimated pre/post decode BER indicators

## 14. Reed-Solomon / TS recovery

Responsibilities:

- RS decoding
- sync-byte validation
- MPEG-TS packet recovery

Diagnostics:

- corrected bytes / packets
- uncorrectable packet count
- sync recovery status
- continuity counter anomalies

## 15. Output writer

Responsibilities:

- write valid TS packets to `stdout`
- optionally suppress output until stable lock is reached

Diagnostics:

- TS packets written
- bitrate estimate
- continuity warnings

## Runtime state machine

The receiver should have an explicit state machine:

```text
INIT
  -> INPUT_OK
  -> ACQUIRE
  -> COARSE_LOCK
  -> TRACKING
  -> FEC_LOCK
  -> TS_FLOW
```

Fallbacks:

- `TRACKING -> ACQUIRE` if sync is lost
- `FEC_LOCK -> TRACKING` if bitstream quality collapses
- `TS_FLOW -> TRACKING` if TS sync vanishes

This is important for both development and clean debug output.

## Debug output model

There are three debug levels:

### 1. Summary level

Human-readable periodic status, for example once per second:

```text
state=TRACKING sr=333k gi=1/8 fec=1/2 cfo=-182.4Hz timing=0.91 evm=0.12 viterbi=ok rs_corr=3 ts_pkts=1240
```

### 2. Stage-specific level

Verbose text for a selected subsystem:

```text
[sync] gi_peak=1243.1 avg=133.8 ratio=9.29 sym_ofs=2024 cfo=-186.2Hz
[eq] pilots=131 amp_min=0.72 amp_max=1.31 phase_slope=-0.09rad/bin
[fec] puncture=1/2 vit_metric=0.84 rs_corr_packets=2 rs_fail=0
```

### 3. Artifact level

Optional output files for offline analysis:

- raw constellation dumps
- correlation traces
- channel estimate snapshots
- BER trend logs

## Threading model

The first working version can be single-threaded for simplicity.

Suggested later split:

- thread 1: input
- thread 2: DSP chain
- thread 3: TS writer

But correctness comes before concurrency.

## Repository layout

Suggested structure:

```text
.
├── README.md
├── CONTEXT.md
├── ARCHITECTURE.md
├── DEVELOPMENT.md
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── cli/
│   ├── io/
│   ├── dsp/
│   ├── fec/
│   ├── ts/
│   └── util/
└── testdata/
```

## First minimum viable architecture

The first decodable version should keep assumptions fixed:

- one known SR preset
- one known GI
- one known FEC
- QPSK only
- offline IQ file or stdin replay

That is the fastest route to a working chain.
