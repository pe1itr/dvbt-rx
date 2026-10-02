# rbdvbt_rx Windows release 0.1.5

## Wat is nieuw

- Ook de experimentele 35 kHz-mode is nu te kiezen als `35k` bij
  `DVB-T symbol rate` in de Windows-GUI. De keuze blijft bewaard bij herstart.
- De GUI biedt nu `35k`, `40k`, `150k`, `250k` en `333k`.
- Beide Windows-programma's hebben versie `0.1.5`.

## Download en gebruik

Download `rbdvbt_gui-windows-x64-0.1.5.zip`, pak de volledige zip uit en start
`rbdvbt_gui.exe`. Het pakket bevat de ontvanger, GUI, RTL-SDR-tools, benodigde
DLL's en documentatie voor 64-bit Windows. VLC moet apart geïnstalleerd zijn.

Kies voor 35 kHz bandbreedte `35k`; bij 40 kHz kies je `40k`. Laat
`Sample rate` op de werkelijke samplerate van de SDR of IQ-opname staan.
Bij `--dvbt-ir 1` gebruikt de ontvanger intern respectievelijk 40.000 en
circa 45.714 samples/s. Zender en ontvanger moeten dezelfde bandbreedte,
FEC en guard gebruiken. Beide smalle modes gebruiken in de GUI blokken van
64 OFDM-symbolen, wat bij GI `1/32` circa 3,38 en 2,96 seconden signaal omvat.

De RTL-SDR-binaries komen uit de Osmocom Windows-build
`rtl-sdr-64bit-20260517.zip`; zie `RTLSDR_SOURCE.txt` in het pakket.

## Verificatie en beperkingen

- Linux-build en Windows-cross-build met MinGW en Qt 6 gecontroleerd.
- De decoderondersteuning is ongewijzigd; deze release voegt de ontbrekende
  35 kHz-keuze aan de GUI toe.
- De nieuwe GUI-build moet nog op Windows worden beproefd; ontvangst via RF
  op 35 en 40 kHz moet nog worden getest.
- Gepubliceerd als pre-release, net als de eerdere Windows-pakketten.
