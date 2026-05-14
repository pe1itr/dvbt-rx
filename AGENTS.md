# Agent Instructions

This repository contains `rbdvbt_rx`, a reduced-bandwidth DVB-T receiver for
amateur DATV experiments. Keep changes practical and close to the existing C
and CMake style.

## Project Shape

- Main receiver code lives in `src/` and public/internal headers live in
  `include/`.
- Small helper programs live in `tools/`.
- User-facing documentation is `README.md`; implementation notes and pipeline
  design live in `ARCHITECTURE.md`.
- `docs/`, `recordings/`, `other_parties/`, `build*`, and `dist/` are ignored
  local or generated trees. Use them for context when present, but do not treat
  them as normal source files to edit or commit unless explicitly asked.
- `other_parties/portsdown4`, `other_parties/leansdr`, and
  `other_parties/gr-dvbt` are external reference trees. Do not refactor or
  reformat them as part of receiver changes.

## Build And Verification

The normal build is:

```sh
cmake -S . -B build
cmake --build build -j
```

The project expects:

- CMake 3.16 or newer
- C compiler with C11 support
- C++ compiler with C++11 support
- `pkg-config`
- FFTW3 single precision development package (`fftw3f`)
- Optional X11 development headers/libraries for GUI support

There is no dedicated test suite at the time of writing. For code changes,
at least run the CMake build when dependencies are available. For receiver
behavior changes, prefer a small reproducible command with `--max-samples` or
an existing IQ capture if the user provides one.

## Receiver Rules

- When `--stdout-ts` or `--ts-out -` is used, `stdout` must contain only MPEG-TS
  packet bytes.
- All logs, progress, diagnostics, and status text must go to `stderr` or to an
  explicit output file such as `--status-json`.
- Keep the receiver narrow unless the user asks otherwise: QPSK, DVB-T 2K,
  reduced-bandwidth modes, and explicit CLI parameters are the current design
  center.
- TPS autodetection is not currently part of the lock chain. Do not assume it
  exists when changing configuration or acquisition logic.
- The `--probe-constellation` name is historical. In the current code it is the
  practical path for the full demodulation chain, not just a diagnostic probe.

## Coding Style

- Follow the surrounding C style. Prefer small, explicit functions over broad
  abstractions.
- Keep C at C11 and C++ at C++11 unless the build configuration is intentionally
  changed.
- Avoid unrelated formatting churn, especially in DSP-heavy code where small
  changes can obscure behavior.
- Put shared declarations in `include/` and keep file-local helpers `static`.
- Preserve clean separation between data output, diagnostics, and monitoring.

## Documentation

- Update `README.md` when user-facing CLI behavior, supported modes, or build
  requirements change.
- Update `ARCHITECTURE.md` when the decode chain, module responsibilities, or
  planned pipeline architecture changes.
- Keep examples copy-pasteable and make clear whether a command writes MPEG-TS
  to `stdout`, to a file, or to a player pipeline.
