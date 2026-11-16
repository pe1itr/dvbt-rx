# Receiver sources for dvbt-rx

This source subset comes from https://github.com/pe1itr/sdr-dvb-pipelines,
revision b4a6f7e (2026-11-16), including the live resampler history fix.
It contains the DVB-T component, SRT supervisor, their licenses and the tests
used by this repository. Other upstream receivers and the GTK application
are not included.

Local changes include a C11-compatible NEON table initializer parameter to
avoid nested-array qualifier warnings on the ODROID compiler.

These files are tracked in dvbt-rx so a normal git pull and local rebuild on
the ODROID includes decoder fixes without updating a second repository.
Make decoder changes here as part of the same dvbt-rx commit. When syncing
with linrad_dvb, compare and explicitly transfer relevant source changes;
CMake never downloads or refreshes this directory automatically.
