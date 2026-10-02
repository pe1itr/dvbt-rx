# rbdvbt_rx Windows release 0.1.4

## Wat is nieuw

- Experimentele 40 kHz DVB-T-bandbreedte, te kiezen als `40k` in de Windows
  GUI bij `DVB-T symbol rate`. De decoder accepteert `--sr 40k`, `40ks` en
  `40000`.
- De decoder ondersteunt ook de experimentele 35 kHz-mode via `--sr 35k`.
- Nieuwe Windows-binaries van `rbdvbt_gui.exe` en `rbdvbt_rx.exe`, beide
  met versie `0.1.4`.

## Download en gebruik

Download `rbdvbt_gui-windows-x64-0.1.4.zip`, pak de volledige zip uit en start
`rbdvbt_gui.exe`. Het pakket is bedoeld voor 64-bit Windows en bevat de
decoder, GUI, RTL-SDR-tools, Qt- en FFTW-runtime-DLL's en documentatie.
VLC wordt niet meegeleverd en moet apart geïnstalleerd zijn.

Kies voor 40 kHz bandbreedte `40k`; laat `Sample rate` op de werkelijke
samplerate van de SDR of IQ-opname staan. De ontvanger resamplet intern naar
circa 45.714 samples/s bij `--dvbt-ir 1`. Zender en ontvanger moeten dezelfde
bandbreedte, FEC en guard gebruiken. Bij deze smalle mode zijn de acquisitie-
en buffertijden langer: 64 OFDM-symbolen duren bij GI `1/32` circa 2,96 seconden.

De meegeleverde RTL-SDR-binaries komen uit de Osmocom Windows-build
`rtl-sdr-64bit-20260517.zip`; zie `RTLSDR_SOURCE.txt` in het pakket.

## Verificatie en beperkingen

- Linux-build en Windows-cross-build met MinGW en Qt 6 gecontroleerd.
- Synthetische 40 kHz IQ-test op Linux met QPSK, FEC `1/2` en GI `1/32`:
  174 pakketten gedecodeerd zonder RS-, synchronisatie- of continuïteitsfouten;
  153 MPEG-TS-pakketten uitgevoerd. Bij TS-uitvoer bevat stdout alleen TS-bytes.
- 40 kHz is experimenteel; ontvangst via RF en de nieuwe Windows-GUI-build
  moeten nog op een Windows-systeem worden beproefd.
- Gepubliceerd als pre-release, net als de eerdere Windows-pakketten.
