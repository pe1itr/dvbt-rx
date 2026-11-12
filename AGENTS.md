# Agent Instructions

This repository contains the Qt6 RTL-SDR interface and builds `rbdvbt_rx`
from the sole decoder source in `sdr-dvb-pipelines/components/dvbt`.
Keep changes practical and close to the existing C/C++ and CMake style.

## Project Shape

- GUI code lives in `gui/`. Decoder sources and headers live in the external
  component selected by `RBDVBT_DECODER_ROOT`; do not create a local decoder fork.
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
- C++ compiler with C++17 support for Qt6 (existing helpers remain C++11)
- `pkg-config`
- FFTW3 single precision development package (`fftw3f`)
- Optional X11 development headers/libraries for GUI support
- Qt6 Widgets/Network/Test for the interface and tests; optional libVLC for video
- A checkout of `sdr-dvb-pipelines` next to this repository (or an explicit
  `RBDVBT_DECODER_ROOT`). No network fetch occurs during configuration.

Run `ctest --test-dir build --output-on-failure` for GUI/process tests and the
available upstream decoder tests. Tests use localhost UDP but no SDR hardware.
For code changes, at least run the CMake build when dependencies are available. For receiver
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
- Keep C at C11 and the helper tools at C++11. The Qt6 interface intentionally
  uses C++17 through its CMake target feature.
- Avoid unrelated formatting churn, especially in DSP-heavy code where small
  changes can obscure behavior.
- Keep GUI declarations in `gui/`; decoder declarations belong to the shared
  component. Keep file-local C helpers `static`.
- Preserve clean separation between data output, diagnostics, and monitoring.

## Documentation

- Update `README.md` when user-facing CLI behavior, supported modes, or build
  requirements change.
- Update `ARCHITECTURE.md` when the decode chain, module responsibilities, or
  planned pipeline architecture changes.
- Keep examples copy-pasteable and make clear whether a command writes MPEG-TS
  to `stdout`, to a file, or to a player pipeline.
