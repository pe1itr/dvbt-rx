# RB-DVB-T Receiver

A lightweight software receiver for reduced-bandwidth amateur DVB-T.

## Goal

This project aims to demodulate a reduced-bandwidth DVB-T signal from a complex IQ stream on `stdin` and output an MPEG transport stream on `stdout`, so it can be piped directly into VLC or other MPEG-TS tools.

Target command-line pattern:

```bash
cat iq.cs16 | rbdvbt_rx --stdin --sample-rate 1010526 --sr 333k --gi 1/8 --fec 1/2 > stream.ts
```

or live:

```bash
afedri_source | rbdvbt_rx --stdin --sample-rate 1010526 --sr 333k --gi 1/8 --fec 1/2 | cvlc - --demux=ts
```

## Scope for v1

Version 1 is intentionally narrow:

- input: interleaved complex `int16` IQ on `stdin`
- sample rate: `1010526 Hz`
- modulation: `QPSK only`
- user-selectable reduced-bandwidth presets:
  - `125k`
  - `150k`
  - `250k`
  - `333k`
  - `500k`
- guard interval:
  - `1/4`
  - `1/8`
  - `1/16`
  - `1/32`
- FEC:
  - `1/2`
  - `2/3`
  - `3/4`
- output: MPEG-TS on `stdout`
- strong focus on observability and debug output at every DSP stage

## Non-goals for v1

The following are explicitly out of scope for the first version:

- DVB-T2
- 16QAM / 64QAM
- hierarchical modulation
- automatic scanning of unknown signals
- GUI
- GNU Radio integration
- hardware-specific receiver APIs in the main datapath

## Design philosophy

This project follows a few strict principles:

1. Keep the receiver lightweight and pipeline-oriented.
2. Prefer fixed, known amateur RB-DVB-T modes over full generality.
3. Make every stage measurable and debuggable.
4. Start with offline and deterministic debugging before live SDR work.
5. Expose useful terminal diagnostics without breaking `stdout` MPEG-TS output.

That means:

- transport stream bytes go only to `stdout`
- logs and debug text go only to `stderr`
- optional debug artifacts can be written to files

## User-visible parameters

The receiver will support a CLI similar to:

```bash
rbdvbt_rx \
  --stdin \
  --sample-rate 1010526 \
  --sr 333k \
  --gi 1/8 \
  --fec 1/2 \
  --debug sync,pilots,eq,tps,fec,ts \
  --stats 1
```

## Debugging requirements

This project is not only about decoding. It is also a tool for development and tuning.

For each major stage, the user must be able to inspect whether it behaves correctly:

- input level and clipping
- DC offset
- coarse frequency offset estimate
- symbol timing / guard correlation quality
- FFT bin energy overview
- pilot extraction quality
- channel estimate stability
- constellation quality after equalization
- deinterleaver consistency checks
- Viterbi lock / bit metrics
- Reed-Solomon correction counts
- TS sync and continuity

## Suggested output usage

Because transport stream data is written to `stdout`, debug must go to `stderr`.

Examples:

```bash
afedri_source | rbdvbt_rx --stdin --sample-rate 1010526 --sr 333k --gi 1/8 --fec 1/2 1>stream.ts 2>debug.log
```

```bash
afedri_source | rbdvbt_rx --stdin --sample-rate 1010526 --sr 333k --gi 1/8 --fec 1/2 2>debug.log | cvlc - --demux=ts
```

## Project status

This repository starts as a design-first project. The first milestone is not a full receiver, but a validated software framework with:

- repeatable CLI behavior
- deterministic test inputs
- clear module boundaries
- measurable debug output

See:

- `CONTEXT.md` for the project background and constraints
- `ARCHITECTURE.md` for the software design
- `DEVELOPMENT.md` for the implementation strategy
