# RB-DVB-T QPSK Probe

This is a **first-stage offline analyzer** for a recorded reduced-bandwidth DVB-T IQ capture.

It is intentionally **not yet a full MPEG-TS decoder**.
Its job is to answer a narrower question first:

- can the capture be framed as OFDM with the selected guard interval?
- which FFT size looks most plausible?
- after a simple per-carrier fourth-power equalization, does the result look like valid QPSK?

That makes it suitable as the first validation tool for a 150 ks RB-DVB-T recording.

## What it does

Input:

- interleaved complex `int16` IQ
- little-endian
- from file or `stdin`

Processing:

1. read and normalize IQ
2. remove mean DC offset
3. sweep candidate FFT sizes
4. detect symbol timing from guard-interval correlation
5. estimate coarse CFO from GI phase
6. FFT a number of OFDM symbols
7. find active bins
8. estimate a simple per-bin channel term using QPSK 4th-power statistics
9. aggregate equalized constellation points
10. compute a QPSK plausibility score

Outputs:

- report on `stderr` by default
- constellation CSV and SVG
- spectrum CSV and SVG

## Build

Requirements:

- `g++`
- `cmake`
- C++17

Build:

```bash
cd rbdvbt_qpsk_probe
cmake -S . -B build
cmake --build build -j
```

Binary:

```bash
./build/rbdvbt_qpsk_probe
```

## Usage

### Analyze a file

```bash
./build/rbdvbt_qpsk_probe \
  --input capture.cs16 \
  --sample-rate 1010526 \
  --sr 150k \
  --gi 1/8 \
  --artifact-prefix test150 \
  --debug input,sync,qpsk
```

### Analyze from stdin

```bash
cat capture.cs16 | ./build/rbdvbt_qpsk_probe \
  --stdin \
  --sample-rate 1010526 \
  --sr 150k \
  --gi 1/8 \
  --artifact-prefix test150 \
  --debug input,sync,qpsk
```

### Restrict the FFT candidates

If you want a tighter hypothesis set:

```bash
./build/rbdvbt_qpsk_probe \
  --input capture.cs16 \
  --sample-rate 1010526 \
  --sr 150k \
  --gi 1/8 \
  --fft-candidates 512,1024,2048 \
  --artifact-prefix test150
```

## Interpreting the report

The most important values are:

- `gi_corr_ratio`
  - higher is better
  - around `> 2` is interesting
  - around `> 4` is a much stronger OFDM timing clue
- `qpsk_score`
  - `>= 75`: strongly plausible QPSK
  - `55 .. 75`: possibly valid QPSK
  - `35 .. 55`: weak / marginal evidence
  - `< 35`: not convincing

The visual artifact is usually even more informative:

- `*_constellation.svg`
- `*_spectrum.svg`

If the constellation clusters near the 4 QPSK points and the GI correlation is clearly peaked, then the recording is a good candidate for the next development step.

## Important limitation

This tool does **not** yet do:

- pilot-aware DVB-T carrier mapping
- TPS interpretation
- deinterleaving
- Viterbi
- Reed-Solomon
- MPEG-TS output

So this is a **probe**, not yet the full receiver.

## Practical first test for your recording

Start with:

```bash
./build/rbdvbt_qpsk_probe \
  --input capture.cs16 \
  --sample-rate 1010526 \
  --sr 150k \
  --gi 1/8 \
  --fft-candidates 512,1024,2048,4096 \
  --artifact-prefix test150 \
  --debug input,sync,qpsk
```

Then inspect:

- the reported `best_fft_size`
- `gi_corr_ratio`
- `qpsk_score`
- `test150_constellation.svg`

If that already looks convincing, the next step is to make the analyzer more DVB-T-specific:

- proper carrier map
- pilot extraction
- common phase correction
- real data carrier selection

and only after that:

- deinterleaving and FEC
