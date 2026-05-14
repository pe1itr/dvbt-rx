# rbdvbt_gui voor Windows

`rbdvbt_gui.exe` is een Windows 11 launcher voor live DVB-T ontvangst met
RTL-SDR, `rbdvbt_rx.exe` en VLC. De GUI start de drie processen direct en
verbindt de datastroom programmatisch:

```text
rtl_sdr.exe stdout -> rbdvbt_rx.exe stdin
rbdvbt_rx.exe MPEG-TS UDP 127.0.0.1:10000 -> VLC udp://@:10000
```

Er wordt geen `cmd.exe`, batchfile of shell-pipe gebruikt. De transportstream
loopt via UDP omdat VLC stdin-pipes op Windows 11 niet overal betrouwbaar zijn.

## Benodigd

- Windows 11
- Qt 6 met Widgets
- CMake 3.16 of nieuwer
- Een Windows toolchain die Qt 6 kan bouwen
- `rtl_sdr.exe`
- `rbdvbt_rx.exe`
- `vlc.exe`
- `librtlsdr.dll` of `rtlsdr.dll`
- `libusb-1.0.dll`

## Build

Configureer op Windows met een Qt 6 installatie in `CMAKE_PREFIX_PATH`.
Voorbeeld:

```bat
cmake -S . -B build-win -DCMAKE_PREFIX_PATH=C:\Qt\6.6.3\msvc2019_64
cmake --build build-win --config Release --target rbdvbt_gui
```

De target `rbdvbt_gui` wordt alleen onder `WIN32` aangemaakt. De normale Linux
build van `rbdvbt_rx` krijgt geen Qt-afhankelijkheid.

## Packaging

Maak een map met minimaal:

```text
rbdvbt_gui.exe
rbdvbt_rx.exe
rtl_sdr.exe
librtlsdr.dll
libusb-1.0.dll
README_WINDOWS_GUI.txt
```

Voeg daarnaast de Qt runtime DLL's toe. Gebruik bij een Qt installatie meestal:

```bat
windeployqt path\to\package\rbdvbt_gui.exe
```

VLC wordt niet meegeleverd. De GUI gaat ervan uit dat VLC al op de Windows
laptop is geinstalleerd. VLC mag naast de GUI staan als `vlc.exe`, maar de GUI
zoekt ook standaard in:

```text
C:\Program Files\VideoLAN\VLC\vlc.exe
C:\Program Files (x86)\VideoLAN\VLC\vlc.exe
```

De standaardinstellingen komen overeen met deze `cmd.exe`/batch commandline.
Start VLC eerst; de decoder-pipe blijft live draaien en keert normaal niet
terug zolang de ontvangst loopt.

```bat
cd /d C:\HamRadio\rbdvbt_gui-windows-x64-0.1.2
start "" "C:\Program Files\VideoLAN\VLC\vlc.exe" udp://@:10000
rtl_sdr.exe -f 437000000 -s 1010526 -g 30 - | rbdvbt_rx.exe --stdin --live --resample-to-dvbt-rate --input-format u8 --sample-rate 1010526 --sr 250000 --gi 1/32 --fec 2/3 --udp-out 127.0.0.1:10000 --wait-video-start --loglevel info
```

De GUI voert dezelfde pipeline uit zonder batchfile en zonder shell-pipe, en
start VLC zelf met `udp://@:10000`.

## Gebruik

1. Start `rbdvbt_gui.exe`.
2. Controleer de standaard ontvangstinstellingen:
   - frequentie: `437000000`
   - RTL sample rate: `1010526`
   - RTL gain: `30`
   - symbol rate: `250000`
   - guard interval: `1/32`
   - FEC: `2/3`
   - loglevel: `info`
3. Open `Configuratie` als de paden naar `rtl_sdr.exe`, `rbdvbt_rx.exe` of
   `vlc.exe` niet automatisch gevonden zijn.
4. Klik `Check installatie`.
5. Klik `START`.
6. Klik `STOP` om VLC, decoder en RTL-SDR in die volgorde te stoppen.

De video wordt in het hoofdvenster embedded via VLC `--drawable-hwnd`.
Het statuspaneel toont live OFDM lock, SNR, service name en provider name op
basis van de decoderlog.

### IQ-bestand afspelen

Gebruik `Bestand > Open IQ bestand...` of de knop `Open` naast het IQ-bestand
veld. Kies daarna:

- `Input`: `IQ bestand`
- `IQ formaat`: `u8` of `s16`
- `Sample rate`: de sample-rate waarmee het IQ-bestand is opgenomen
- `DVB-T symbol rate`, `Guard interval`, `FEC` en `Decoder loglevel`

In deze modus wordt `rtl_sdr.exe` niet gestart en zijn `librtlsdr.dll` /
`rtlsdr.dll` en `libusb-1.0.dll` niet nodig. De GUI voert het bestand naar
`rbdvbt_rx.exe` via stdin en stuurt de transportstream via UDP naar VLC. Het bestand
wordt gepaced op de ingestelde sample-rate; `u8` telt als 2 bytes per complex
IQ-sample en `s16` als 4 bytes per complex IQ-sample.

## Diagnose

Het onderste paneel toont logs voor:

- Alles
- RTL-SDR
- Decoder
- VLC
- Diagnose

Gebruik `Kopieer diagnose` om supportinformatie naar het clipboard te kopieren.
De tekst bevat de GUI versie, Windows versie, ingestelde paden, commandlines,
dependency check, processtatussen, byte counters en de laatste logregels.
