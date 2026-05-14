# rbdvbt_rx Windows release 0.1.2

## Wat is nieuw

- Nieuwe Windows GUI launcher `rbdvbt_gui.exe`.
- Nieuwe UDP transportstream-uitgang voor VLC:
  `--udp-out 127.0.0.1:10000`.
- De GUI start de keten zonder batchfile of shell-pipe:
  `rtl_sdr.exe -> rbdvbt_rx.exe -> UDP -> VLC`.
- `rbdvbt_rx.exe` zet Windows `stdin` en `stdout` expliciet in binary mode,
  zodat IQ-data uit `rtl_sdr.exe` niet door text-mode conversie beschadigd raakt.
- Verbeterde live recovery bij zwakke of haperende signalen.
- Outer decoder reset minder snel bij korte pilot-lock dips, zodat bruikbare
  MPEG-TS packets sneller bij VLC/ffplay aankomen.
- Versie verhoogd naar `0.1.2`.

## Package

Bestand:

```text
dist/rbdvbt_gui-windows-x64-0.1.2.zip
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

De commandline-equivalent is:

```bat
rtl_sdr.exe -f 437000000 -s 1010526 -g 30 - | rbdvbt_rx.exe --stdin --live --input-format u8 --sample-rate 1010526 --sr 250000 --gi 1/32 --fec 2/3 --udp-out 127.0.0.1:10000 --wait-video-start --loglevel info
vlc.exe udp://@:10000
```

## Te testen

- Live ontvangst via RTL-SDR en VLC.
- IQ-bestand afspelen via de GUI.
- `150k` en `250k` symbol rate.
- FEC `1/2` en `2/3`.
- Zwakke signalen rond 4.5-6 dB SNR.
- Of VLC snel beeld toont bij tijdelijke RS-fouten of korte signaaldips.

## Bekende aandachtspunten

- Bij zwakke signalen kan de status tijdelijk `DEGRADED` tonen terwijl VLC toch
  bruikbaar beeld laat zien.
- De decoder kan bij acquire eerst een foute cadence proberen en daarna snel
  opnieuw locken.
- `stdout` blijft alleen MPEG-TS als `--stdout-ts` of `--ts-out -` wordt
  gebruikt. Diagnostiek gaat naar `stderr`.
- UDP output verwacht een IPv4 doel, bijvoorbeeld `127.0.0.1:10000`.

## Verificatie

Gebouwd met de MinGW Windows cross-toolchain en Qt 6 runtime. Beide binaries
bevatten versie `0.1.2`.
