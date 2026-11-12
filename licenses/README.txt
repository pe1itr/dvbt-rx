License files and binary releases
================================

The project's LICENSE is the unmodified GNU GPL version 3 text. NOTICE applies
GPL-3.0-only to project-owned code and documentation and identifies the author.
This includes the Qt GUI, the SRT wrapper and the integration tests.

DECODER-GPL-3.0.txt is an identical copy retained for the external DVB-T decoder.
Attribution-legacy.txt preserves the previous project license for historical
versions; it is not an additional condition on the current GPL-licensed code.

GPL-2.0.txt, LGPL-2.1.txt and LGPL-3.0.txt contain standard license texts for
dependencies where applicable. Copies were taken from the Qt6 license-text
distribution. The actual dependency version, its copyright notices and its
license grant determine which terms apply; copying these texts does not
relicense a dependency or replace its own notices.

Before publishing a binary release:

- Preserve LICENSE, NOTICE and this licenses directory in the package.
- Identify the exact project and sdr-dvb-pipelines source used, including local
  modifications. Supply the corresponding source and build/install scripts
  alongside the binary download, or use another method allowed by the license.
  A link to a moving branch or to another binary package does not identify the
  exact source. Both components/dvbt and src/srt_output.* / src/process_control.h
  are external sources needed when their respective programs are included.
- Preserve the original copyright and license notices for every bundled Qt,
  FFTW, GLib, RTL-SDR, libusb, VLC, FFmpeg or compiler-runtime component. Include
  any additional component-specific license files and meet its source and
  installation/relinking requirements where applicable.
- Match the dependency sources to the actual shipped binaries and build
  configuration. FFmpeg's applicable terms, for example, depend on its build.

The Windows packaging script copies the project license texts and guidance;
the release publisher still supplies the exact corresponding sources and
dependency-specific notices. Historical packages in dist/ are not rewritten
by a license change in the current source tree.

Upstream licensing references:
  GNU GPL v3: https://www.gnu.org/licenses/gpl-3.0.html
  Qt: https://www.qt.io/development/open-source-lgpl-obligations
  Shared decoder: https://github.com/pe1itr/sdr-dvb-pipelines
