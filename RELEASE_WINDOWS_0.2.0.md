# Windows release 0.2.0

Deze pre-release bevat de nieuwe Qt6-interface voor RTL-SDR en DVB-T 2K QPSK.
De verdere ontwikkeling vindt plaats in
[sdr-dvb-pipelines](https://github.com/pe1itr/sdr-dvb-pipelines).

## Wijzigingen

- Spectrum en waterval over de volle breedte, met constellatie,
  ontvangststatus, FIFO-belasting en video-/spelerbediening eronder.
- Frequentie en gain tijdens ontvangst aanpassen met de pijltjestoetsen;
  asynchroon opnieuw afstemmen en nette procesopruiming.
- De enige decoderbron is nu `sdr-dvb-pipelines/components/dvbt`.
  Streaming-Viterbi ondersteunt alle FEC-rates van 1/2 tot en met 7/8, met
  automatische AVX2-selectie op geschikte Windows x64-processors.
- Live AFC is ingeschakeld. IQ-overruns tussen frontendblokken worden correct
  als sampleverlies gemeld aan de decoder.
- Eigen projectcode onder GPL-3.0-only, met behoud van copyright
  Rob Hardenberg / PE1ITR en de licentiegegevens van externe componenten.
- Headless SRT en ODROID-N2-buildprofielen zijn beschikbaar in de Linux-broncode.

## Download en gebruik

Download `rbdvbt_gui-windows-x64-0.2.0.zip`, pak de volledige map uit en start
`rbdvbt_gui.exe`. Het pakket bevat de benodigde Qt6/MinGW-DLL's en RTL-SDR-tools.

Deze Windows-build gebruikt **Externe speler** voor video/audio. Installeer
VLC apart en kies zo nodig `vlc.exe` bij Instellingen. De instellingen van de
oude 0.1.x-launcher worden niet automatisch overgenomen.

De GUI ondersteunt 35, 40, 125, 150, 250, 333 en 500 kHz. Ook 35/40/125 kHz
zijn uitgebreid geteste modes en standaard onderdeel van `linrad_dvb`.
Het [Luxemburg-verslag](https://pe1itr.com/144mhz/144mhz-40khz-dvb-t-experiment-luxembourg-2026.htm)
beschrijft 40 kHz-ontvangst met de Windows-software uit deze repository.
Stel samplerate, bandbreedte, FEC en guard in overeenkomstig de IQ-bron en zender.

## Broncode en licenties

- `rbdvbt_gui-windows-x64-0.2.0-source.zip`: de projectbron, de exacte gebruikte
  gedeelde decoder/SRT-bron en de RTL-SDR-bron met bouwinstructies.
- `rbdvbt_gui-windows-x64-0.2.0-dependency-sources.zip`: de bij de meegeleverde
  DLL's behorende Fedora-bronpakketten, inclusief patches en buildrecepten.
- `SHA256SUMS.txt`: controlesommen van de downloads.

De bronpakketten zijn niet nodig om de Windows-programma's te gebruiken.
`RUNTIME-SOURCES.json` in het runtimepakket identificeert de meegeleverde
bestanden, hun hashes en bronpakketten. Zie LICENSE, NOTICE en licenses/ voor
de toepasselijke licenties en copyrightvermeldingen.

## Verificatie en beperkingen

- Windows x64-cross-build met MinGW/GCC en Qt 6.10.3.
- Tien Windows-GUI/procestests geslaagd onder Wine, inclusief starten, stoppen,
  herafstemming, visualisatie en het wegvallen van bron/decoder.
- Windows-decodertests voor streaming-FEC (SIMD en scalar), PSI en frontend
  geslaagd onder Wine. De Windows-binary levert bij de 250/333 kHz-IQ-replays
  byte voor byte dezelfde MPEG-TS als de Linux-binary.
- Linux-build en hardwarevrije decoder-/proces-/SRT-tests gecontroleerd.
- Native Windows-ontvangst met fysieke RTL-SDR-hardware en een ODROID-N2-duurtest
  zijn niet uitgevoerd voor deze release. Daarom blijft dit een pre-release.
