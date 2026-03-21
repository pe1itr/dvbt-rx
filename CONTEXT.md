# Context

## Project motivation

The goal is to build a lightweight software receiver for reduced-bandwidth amateur DVB-T that behaves more like a Unix pipeline tool than a large framework.

Desired behavior:

- read IQ from `stdin`
- decode RB-DVB-T in software
- emit MPEG transport stream on `stdout`
- connect directly to VLC or TS analysis tools

This project exists because the preferred style is:

```text
IQ stream in -> decoder -> MPEG-TS out
```

rather than a GNU Radio flowgraph or a hardware-specific DVB-T demodulator board.

## Practical origin

In amateur DATV practice, reduced-bandwidth DVB-T modes are commonly used at values such as:

- 125k
- 150k
- 250k
- 333k
- 500k

The user-facing terminology often refers to these as “symbol rates”, even though internally DVB-T is an OFDM system and the software must translate those presets into appropriate timing and demodulation constants.

For this project, that user vocabulary is kept intentionally because it matches operational practice.

## Input and output constraints

The project is built around a fixed live input contract:

- sample rate: `1010526 Hz`
- sample format: interleaved signed 16-bit complex IQ
- source: `stdin`

and a fixed output contract:

- MPEG-TS on `stdout`

That leads to one critical engineering rule:

- all logs and debug information must go to `stderr`

## Why this project is intentionally narrow

A full DVB-T receiver is a large undertaking. A narrow receiver for a specific amateur RB-DVB-T subset is much more realistic.

To keep the work tractable, the project starts with:

- QPSK only
- selected reduced-bandwidth presets only
- manual selection of GI and FEC
- strong instrumentation

This is not a consumer DVB-T stick replacement.

It is a specialist amateur DATV demodulator focused on a known subset.

## Development priorities

The project has two equally important goals:

### 1. decode the signal

The receiver must eventually produce valid MPEG-TS.

### 2. understand the signal path

Each DSP stage must expose enough information to determine whether it is behaving correctly.

That means the project should not be developed as a black box. It should be developed as an observable DSP instrument.

## Debugging expectations

The user explicitly wants to inspect the quality of each step in the chain. Therefore, the software should support:

- terminal debug output per subsystem
- periodic summary status
- optional logging to file
- optional dumps for offline analysis

Useful examples include:

- symbol timing correlation values
- coarse and fine frequency offset
- pilot quality and stability
- equalized constellation spread
- FEC correction counters
- TS packet lock and continuity

## Usability target

The final user experience should resemble existing lightweight SDR/Linux pipelines.

Examples:

```bash
some_iq_source | rbdvbt_rx --stdin --sample-rate 1010526 --sr 150k --gi 1/16 --fec 3/4 | cvlc - --demux=ts
```

```bash
cat capture.cs16 | rbdvbt_rx --stdin --sample-rate 1010526 --sr 333k --gi 1/8 --fec 1/2 > test.ts
```

## Language choice context

Although the user has no C++ experience, the expected throughput and DSP-heavy nature of the receiver make C++ a reasonable implementation language.

The project should therefore be structured to reduce C++ friction by emphasizing:

- small modules
- predictable data flow
- explicit interfaces
- minimal template complexity
- strong logging
- incremental milestones

In other words, the software should be written in a C-like, disciplined C++ style rather than in an overly abstract modern C++ style.

## Project character

This project is best viewed as:

- a focused RB-DVB-T demodulator
- a software-defined receive chain
- a debugable DSP tool
- a staged engineering project, not a quick script

The first success criterion is not elegance. It is observability and correctness on one known mode.
