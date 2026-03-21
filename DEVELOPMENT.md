# Development

## Development strategy

Build this project in narrow, testable steps.

Do not start by aiming for a full end-to-end live receiver with all modes at once.

Instead:

1. establish the shell of the program
2. make input and debug behavior reliable
3. add one DSP block at a time
4. validate every stage before proceeding
5. only then attempt full transport stream decoding

## Toolchain

Recommended baseline:

- C++17
- CMake
- FFTW3 or kissfft
- `-O2` or `-O3` for normal builds
- optional `-fsanitize=address,undefined` for debug builds

Suggested build profiles:

- `Debug`
- `RelWithDebInfo`
- `Release`

## Coding style

Prefer a simple style:

- explicit structs
- explicit ownership
- minimal inheritance
- avoid unnecessary templates
- small translation units
- easy-to-read logging

This project should feel like DSP engineering code, not like a framework.

## Output discipline

Keep this rule everywhere:

- `stdout` = MPEG-TS bytes only
- `stderr` = all logs, debug, stats, warnings, errors

Never print debug text to `stdout`.

## Milestones

## Milestone 0: repository skeleton

Goal:

- repository structure exists
- program starts
- CLI parses arguments
- `stdin` IQ can be read
- stats can be printed to `stderr`

Deliverables:

- `main.cpp`
- argument parser
- stdin reader
- config dump to `stderr`
- sample counters

Validation:

```bash
cat iq.cs16 | rbdvbt_rx --stdin --sample-rate 1010526 --sr 333k --gi 1/8 --fec 1/2 > /dev/null
```

Expected:

- no crash
- sample counters visible on `stderr`
- no text on `stdout`

## Milestone 1: front-end instrumentation

Goal:

- parse IQ correctly
- estimate DC offset
- estimate power and clipping
- verify stream stability

Deliverables:

- complex sample conversion
- DC blocker
- basic level stats
- optional IQ dump snippets

Validation:

- compare measured mean and power on known recordings
- confirm no byte-order mistakes

## Milestone 2: resampling and mode table

Goal:

- support all requested user presets as configuration modes
- convert input stream at `1010526 Hz` to internal processing rate

Deliverables:

- mode table for `125k`, `150k`, `250k`, `333k`, `500k`
- resampler
- timing ratio diagnostics

Validation:

- synthetic tone tests
- known bandwidth occupancy checks

## Milestone 3: acquisition and symbol timing

Goal:

- detect OFDM symbol boundaries using guard interval correlation
- estimate coarse CFO

Deliverables:

- correlation engine
- lock metric
- timing tracker

Validation:

- log correlation peaks
- confirm stable timing on captured RB-DVB-T recordings
- inspect lock persistence over time

Useful debug examples:

```text
[sync] gi=1/8 peak=1289 avg=132 ratio=9.77 cfo=-184.3Hz lock=1
```

## Milestone 4: FFT and carrier inspection

Goal:

- convert timed OFDM symbols into frequency bins
- inspect carrier energy and pilot locations

Deliverables:

- FFT wrapper
- symbol counter
- spectrum/bin diagnostics

Validation:

- verify expected occupied bins
- confirm pilot structure is plausible

## Milestone 5: fine tracking and equalization

Goal:

- track residual phase/frequency error
- estimate and equalize the channel

Deliverables:

- fine phase tracker
- pilot-based channel estimate
- 1-tap equalizer
- constellation debug output

Validation:

- post-equalization QPSK cluster tightening
- stable phase rotation compensation

Useful debug examples:

```text
[eq] amp=[0.81..1.24] phase_slope=-0.07 evm_est=0.14
[qpsk] cluster_spread_i=0.11 cluster_spread_q=0.12
```

## Milestone 6: demapper and deinterleaver

Goal:

- convert equalized carriers into bitstream form
- reverse DVB-T interleaving stages

Deliverables:

- QPSK demapper
- bit/symbol deinterleaver
- sanity counters

Validation:

- deterministic outputs on synthetic tests
- bitflow consistency checks before FEC

## Milestone 7: Viterbi decoding

Goal:

- support puncturing for:
  - `1/2`
  - `2/3`
  - `3/4`

Deliverables:

- Viterbi decoder integration
- puncture patterns
- decoder metrics

Validation:

- unit tests with known encoded sequences
- reasonable BER improvement relative to pre-FEC estimates

## Milestone 8: RS and TS recovery

Goal:

- recover valid MPEG transport stream packets
- verify packet sync and continuity

Deliverables:

- RS decoder
- TS packet checker
- stdout writer

Validation:

- repeated `0x47` sync alignment
- VLC can open the stream
- TS analysis tools report valid structure

## Milestone 9: live-pipeline robustness

Goal:

- decode from continuous live stdin stream
- survive brief degradations

Deliverables:

- state machine transitions
- lock recovery behavior
- stable stderr summary reporting

Validation:

- long live runs
- recover from short fades or disturbances

## Test philosophy

Use three kinds of tests.

### 1. Unit tests

For small deterministic blocks:

- puncturing
- deinterleaving
- packet sync logic
- config parsing

### 2. Recorded IQ regression tests

Keep short captured test files for known-good RB-DVB-T examples. These are essential.

For each file, document:

- preset `sr`
- `gi`
- `fec`
- expected lock behavior
- expected TS output success

### 3. Synthetic tests

Use generated signals where possible to validate isolated DSP blocks.

## Recommended debug switches

Examples:

- `--debug input`
- `--debug sync`
- `--debug fft`
- `--debug pilots`
- `--debug eq`
- `--debug qpsk`
- `--debug fec`
- `--debug ts`
- `--stats 1`

Combined:

```bash
cat iq.cs16 | rbdvbt_rx --stdin --sample-rate 1010526 --sr 250k --gi 1/8 --fec 2/3 --debug sync,eq,fec,ts --stats 1 > out.ts
```

## Practical order of implementation

Recommended real-world order:

1. stdin reader and config
2. stderr logging system
3. DC/power stats
4. resampler
5. GI correlation
6. FFT
7. pilot inspection tools
8. equalizer
9. QPSK demapper
10. deinterleaver
11. Viterbi
12. RS + TS out
13. lock state machine
14. live stability improvements

## Definition of done for first usable version

The first usable version is reached when all of these are true:

- reads live IQ from `stdin`
- accepts one known RB-DVB-T mode preset
- locks symbol timing reliably
- equalized QPSK constellation looks correct
- produces valid TS packets on `stdout`
- VLC plays the transport stream
- useful debug output is available on `stderr`

That is enough for a meaningful first release.
