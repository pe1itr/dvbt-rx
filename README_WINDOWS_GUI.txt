rbdvbt_gui 0.2 — Qt6 DVB-T / RTL-SDR

Dezelfde interface wordt voor Linux en Windows gebouwd. De enige decoderbron
is sdr-dvb-pipelines/components/dvbt. Er is geen lokale decoderfork meer.

Build (MinGW-w64 met pthreads, Qt6, FFTW3f, pkg-config en CMake):

  cmake -S . -B build-win -DRBDVBT_DECODER_ROOT=C:/src/sdr-dvb-pipelines/components/dvbt -DBUILD_TESTING=OFF
  cmake --build build-win --config Release -j

C is C11; de Qt6-interface vereist C++17. Gebruik dezelfde architectuur en
compatibele toolchains voor Qt, FFTW, rtl_sdr en VLC. De decoderbron moet
vooraf aanwezig zijn; de rest van sdr-dvb-pipelines hoeft niet gebouwd te worden.

Runtimebestanden naast elkaar:
  rbdvbt_gui.exe
  rbdvbt_rx.exe
  rtl_sdr.exe (of expliciet pad instellen)
  Qt6 runtime en platformplugin (via windeployqt)
  FFTW3f, pthread/compiler-runtime DLLs die de build nodig heeft
  RTL-SDR/libusb DLLs behorend bij rtl_sdr.exe
  LICENSE, NOTICE en de volledige licenses/ map

  windeployqt path/to/package/rbdvbt_gui.exe

Optioneel: bouw met libvlc via pkg-config voor video/audio in het hoofdvenster.
Daarbij zijn libvlc.dll, libvlccore.dll en de bijbehorende VLC-plugins tijdens
uitvoering nodig. Zonder libvlc blijft ontvangst werken; kies Instellingen
om het pad naar vlc.exe te zetten en gebruik Externe speler.

Gebruik:
1. Start rbdvbt_gui.exe; de ontvanger staat aanvankelijk uit.
2. Stel frequentie, gain, bandbreedte, FEC en guard in.
3. Instellingen: RTL-SDR device-index/serienummer, samplerate, ppm en UDP-poort.
4. Start ontvangst. Video gebruikt een eigen lokale UDP-poort; externe VLC
   gebruikt standaard udp://@:10000. Diagnose toont de procesmeldingen.
5. Stop ontvangst om afstemming of instellingen te wijzigen. Afsluiten stopt
   alle door de interface gestarte processen.

De GUI ondersteunt alleen RTL-SDR. IQ-replay blijft via rbdvbt_rx.exe stdin
beschikbaar voor commandlinetests. Instellingen van de oude 0.1.x-launcher
worden niet overgenomen. De oude Bestands-/IQ- en diagnose-uploadmenu's zijn
vervangen door de compacte ontvangstinterface.

Deze ombouw is op Linux en met MinGW voor Windows x64 gebouwd. De Windows-GUI
is onder Wine met tien hardwarevrije proces- en interfacetests gecontroleerd.
Deze build gebruikt een externe VLC-speler; libVLC is niet meegebundeld.
Een native Windows-praktijktest met fysieke RTL-SDR-hardware blijft nodig.
Zie README.md voor afhankelijkheden, decoderrevisie en licentiegegevens.

De eigen projectcode, GUI en hulpmiddelen vallen onder GPL-3.0-only.
Copyright (c) 2026 Rob Hardenberg / PE1ITR. Zie LICENSE en NOTICE.
Bij publicatie van binaries hoort ook de exacte bijbehorende broncode met
buildscripts beschikbaar te zijn volgens de toepasselijke licenties, inclusief
de gebruikte externe decoderbron. Voeg de notices/licenties van alle
meegeleverde DLLs en programma's toe; zie licenses/README.txt.
