# rbdvbt_rx Windows release 0.1.3

## Wat is nieuw

- Nieuwe Windows build van `rbdvbt_rx.exe` en `rbdvbt_gui.exe` met versie
  `0.1.3`.
- De GUI start de live keten met status-JSON en binaire visualizer UDP:
  `--status-json rx_status.json` en `--visualizer-udp 127.0.0.1:10001`.
- De GUI toont live spectrum- en constellatiebeelden uit de decoder naast de
  bestaande statusinformatie.
- De procestabel is verplaatst naar het tabblad `Processen`; het voorscherm
  toont alleen gekleurde OFDM lock, SNR, service, provider, input bytes en TS
  bytes.
- `TS bytes` gebruikt de bestaande cumulatieve status-JSON teller
  `ts_session_written_packets`, niet de decoderlog.
- Start/stop staan onder de statusinformatie, samen met presets voor
  `436 MHz` en `437 MHz` op `150k`/`250k` met FEC `1/2`.
- Presetknoppen starten direct bij stilstand en herstarten de keten direct als
  de ontvanger al draait.
- Het hoofdscherm heeft nu alleen `RTL-SDR` als live input; IQ-bestanden worden
  geopend via `Bestand > Open IQ u8 bestand...` of
  `Bestand > Open IQ s16 bestand...`.
- `DVB-T symbol rate` is een keuzelijst met `150k`, `250k` en `333k`.
- Bij `150k` start de GUI de decoder met
  `--live-symbols 128 --probe-symbols 128`; bij `250k` en `333k` met
  `--live-symbols 64 --probe-symbols 64`.
- `Guard interval` en `FEC` zijn beperkt tot de praktische testkeuzes in de
  GUI.
- Het menu `Configuratie` bevat `Configuratie...` en `Check installatie`;
  `Help > Info` toont auteur en GitHub-link.
- Het menu `Logging` bevat de decoder-loglevels en
  `Kopieer decoder logging`.
- De standaard live pipeline blijft:
  `rtl_sdr.exe -> rbdvbt_rx.exe -> UDP -> VLC`.

## Package

Bestand:

```text
dist/rbdvbt_gui-windows-x64-0.1.3.zip
```

In de zip zitten:

- `rbdvbt_gui.exe`
- `rbdvbt_rx.exe`
- Qt runtime DLL's
- FFTW runtime DLL
- `README_WINDOWS_GUI.txt`
- `README_PROJECT.md`
- `ADD_RTLSDR_FILES_HERE.txt`

Niet meegeleverd:

- `rtl_sdr.exe`
- `librtlsdr.dll` of `rtlsdr.dll`
- `libusb-1.0.dll`
- VLC

Plaats de RTL-SDR bestanden naast `rbdvbt_gui.exe`, of stel de paden in via de
GUI. VLC mag geinstalleerd zijn in de standaard VideoLAN map.

## Standaard live pipeline

De GUI gebruikt standaard:

```text
rtl_sdr.exe stdout -> rbdvbt_rx.exe stdin
rbdvbt_rx.exe --udp-out 127.0.0.1:10000
VLC udp://@:10000
```

De commandline-equivalent voor `cmd.exe` of een batchfile is:

```bat
cd /d C:\HamRadio\rbdvbt_gui-windows-x64-0.1.3
start "" "C:\Program Files\VideoLAN\VLC\vlc.exe" udp://@:10000
rtl_sdr.exe -f 437000000 -s 1010526 -g 30 - | rbdvbt_rx.exe --stdin --live --resample-to-dvbt-rate --input-format u8 --sample-rate 1010526 --sr 250k --gi 1/32 --fec 2/3 --live-symbols 64 --probe-symbols 64 --udp-out 127.0.0.1:10000 --wait-video-start --status-json rx_status.json --visualizer-udp 127.0.0.1:10001 --loglevel quiet
```

## Te testen

- Live ontvangst via RTL-SDR en VLC.
- IQ-bestand afspelen via de GUI.
- Statuspaneel met gekleurde OFDM lock, SNR, service name, provider name,
  input bytes en TS bytes.
- Procestabblad naast spectrum en constellatie.
- Spectrum- en constellatiepanelen via visualizer UDP.
- `150k`, `250k` en `333k` symbol rate.
- FEC `1/2` en `2/3`.
- Zwakke signalen rond 4.5-6 dB SNR.

## Bekende aandachtspunten

- Bij zwakke signalen kan de status tijdelijk `DEGRADED` tonen terwijl VLC toch
  bruikbaar beeld laat zien.
- `stdout` blijft alleen MPEG-TS als `--stdout-ts` of `--ts-out -` wordt
  gebruikt. Diagnostiek gaat naar `stderr`.
- UDP output verwacht een IPv4 doel, bijvoorbeeld `127.0.0.1:10000`.
- De zip bevat geen `rtl_sdr.exe`, RTL-SDR DLL's of VLC.

## Verificatie

Gebouwd met de MinGW Windows cross-toolchain en Qt 6 runtime. Beide binaries
bevatten versie `0.1.3`.
