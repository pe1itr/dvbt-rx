# Architectuur

## Actuele opzet

Dit project levert een compacte Qt6-interface voor DVB-T-ontvangst met RTL-SDR.
De hoofdvensterindeling volgt `linrad_dvb` uit `sdr-dvb-pipelines`:
bedieningsbalk bovenaan, daaronder een brede waterval en spectrum, en onderaan
constellatie, ontvangststatus en video naast elkaar.
De enige decoderbron is `sdr-dvb-pipelines/components/dvbt`. CMake bouwt die
bronnen rechtstreeks; DSP en FEC worden alleen in die gedeelde component
ontwikkeld. De checkout is nodig tijdens het bouwen, maar niet bij ontvangst.

```text
Qt6 interface
  rtl_sdr stdout (u8 IQ)
    -> rechtstreekse OS-pipe
    -> rbdvbt_rx stdin (gedeelde DVB-T-decoder)
         -> MPEG-TS UDP -> libVLC -> video/audio in het venster
         -> MPEG-TS UDP kopie -> externe VLC/speler
         -> status JSON -> statuspaneel
         -> RBV1 UDP -> spectrum, waterval en QPSK-constellatie
```

Er is één hardwarebron: RTL-SDR. Er is één demodulator: DVB-T 2K QPSK. De GUI
beheert alleen frequentie, gain, nominale bandbreedte, FEC, guard, RTL-device,
samplerate en ppm-correctie. De decoder houdt zijn scriptbare stdin/CLI-interface
voor IQ-opnamen en andere toepassingen.

## Modules

| Bestand | Verantwoordelijkheid |
|---|---|
| `CMakeLists.txt` | Selecteert de gedeelde decoder via `RBDVBT_DECODER_ROOT` en bouwt de GUI en hulpmiddelen. |
| `gui/main.cpp` | Qt6-entrypoint en applicatieversie. |
| `gui/receiver_window.cpp` | Bediening, instellingen, proceslevenscyclus, UDP, status en diagnose. |
| `gui/plots.cpp` | Valideert bestaande RBV1-visualisatieframes en tekent spectrum, waterval en constellatie. |
| `gui/video.cpp` | Optionele libVLC-koppeling voor de ingebedde videoweergave. |
| `tests/test_receiver_ui.cpp` | GUI-, status-, visualisatie- en proceslevenscyclustests. |
| `tests/pipeline_fixture.cpp` | Hardwarevrije IQ-bron en decoderfixture voor tests van de echte OS-pipe. |
| `tools/` | CLI-statusmonitor, IQ-replay en DVB-T-simulatiehulpmiddelen. |

## Proceslevenscyclus en uitvoer

De decoderbinary staat naast de GUI; de GUI start geen decoder uit PATH. De
RTL-tool staat naast de GUI, in PATH of op een ingesteld pad. Alle programma's
starten met argumentlijsten, zonder shell. `QProcess::setStandardOutputProcess`
maakt een OS-pipe tussen `rtl_sdr` en `rbdvbt_rx`; IQ-samples worden niet door de
Qt-eventloop gebufferd. De GUI bewaart alleen decoderstatus en maximaal 1000
regels procesdiagnose.

Start controleert samplerate en bandbreedte, maakt tijdelijke statusopslag en
bindt een lokale UDP-poort voor grafieken. De decoder krijgt een aparte UDP-poort
voor ingebedde video en stuurt een tweede kopie naar de configureerbare externe
spelerpoort (standaard 10000). Spelerproblemen stoppen de ontvangst niet.
Bij Stop, afsluiten of een onverwacht einde van RTL-SDR of decoder wordt de
hele ontvangstketen afgesloten; een proces dat niet binnen de wachttijd stopt,
wordt beëindigd. Status en visualisaties worden daarna gewist. Het openen van
de GUI start geen ontvangst automatisch.

Status die te lang onveranderd blijft wordt als verouderd getoond en een oude
lock wordt gewist. De GUI wacht 6 seconden voor standaardbandbreedtes en
12 seconden bij 35/40 kHz, zodat langere OFDM-blokken niet voortijdig als fout
gelden. Dit verandert niets aan de decoder-locklogica. Op Linux gebruikt
ingebedde libVLC-video X11/XWayland; met native Wayland of zonder libVLC is de
externe speler beschikbaar. Het videovak wordt op Wayland geen native child
window; Qt kan dat anders als een apart scherm tonen.

Tijdens ontvangst stellen ←/→ de frequentie 1 kHz hoger of lager in; ↑/↓
veranderen de gain met 1 dB. Een korte debounce bundelt herhaalde toetsaanslagen.
De interface beëindigt dan de RTL- en decoderprocessen asynchroon, start ze met
de nieuwe afstemming opnieuw en laat VLC en de UDP-poorten actief. Zo blijft de
Qt-eventloop reageren terwijl de DVB-T-decoder de lock opnieuw opbouwt.

## Headless SRT-uitgang

De PI6EHV-launcher houdt RTL-SDR en `rbdvbt_rx` actief tijdens ontvangstverlies:

```text
rtl_sdr -> rbdvbt_rx -> MPEG-TS UDP -> rbdvbt_srt -> FFmpeg -> SRT caller
```

CMake bouwt `rbdvbt_srt` met `src/srt_output.c` uit dezelfde geselecteerde
`sdr-dvb-pipelines`-checkout als de decoder. Deze module is ook de bewaker in
`linrad_dvb`; de lokale `tools/rbdvbt_srt.c` past alleen een optionele IPv4
UDP-ingang aan naar stdin. De checkout is bij runtime niet nodig. Deze
POSIX-helper gebruikt GLib zonder GTK/Qt of scherm en wordt op Linux standaard
meegebouwd (`RBDVBT_BUILD_SRT`). De Windows GUI blijft onafhankelijk hiervan.

De gedeelde module synchroniseert TS, detecteert voortschrijdende,
ongescrambelde videopayload zonder TEI en start alleen dan FFmpeg. Dit is
payload-detectie, geen controle op werkelijk gedecodeerde videoframes. Na
standaard 2000 ms zonder video stopt FFmpeg en dus de SRT-verbinding. De
bewaker blijft TS innemen en start bij terugkerende video opnieuw. Een begrensde
wachtrij van maximaal circa 1 MiB/1 seconde houdt uitgangsproblemen weg van de
ontvangstketen. De launcher stopt de bewaker bij afsluiten; die beëindigt en
reapt zijn encoder, zo nodig met SIGKILL.

De gedeelde FFmpeg-argumenten leveren 1920×1080, standaard 25 fps en 4 Mbit/s
H.264, met deïnterlacing, SAR-bewuste schaling, zwarte balken, vierkante pixels
en optionele AAC stereo 48 kHz/128 kbit/s. Alleen fps, bitrate, preset en
threadpools zijn configureerbaar, gelijk aan `linrad_dvb`. De launcherwatchdog
bewaakt vastgelopen statusupdates na eerste lock; signaalverlies herstart geen
hardware of decoder meer.

## Decoder en verificatie

De decoder ontvangt expliciete DVB-T-modeparameters; TPS-autodetectie maakt
geen deel uit van de lock-keten. `--probe-constellation` blijft de praktische
volledige demodulatieketen. Wanneer TS naar stdout gaat, bevat stdout alleen
MPEG-TS; diagnostiek hoort op stderr of in een expliciet statusbestand.

`ctest --test-dir build --output-on-failure` bouwt de beschikbare gedeelde
streaming-Viterbi-, PSI- en frontendtests rechtstreeks uit de decodercheckout en
test de interface met gecontroleerde subprocessen en localhost UDP. Deze tests
hebben geen RTL-SDR-hardware nodig. Een live RF-praktijktest en een nieuwe
native Windows-build zijn aanvullend nodig voor de betreffende doelmachines.

De SRT-tests hergebruiken de beschikbare upstream bewakertests en testen de
lokale UDP-adapter. Met FFmpeg/ffprobe en SRT-ondersteuning wordt ook werkelijk
via localhost gecodeerd en gecontroleerd op 1080p H.264, AAC, vierkante pixels,
anamorfische 4:3-bronnen, zwarte balken en opnieuw verbinden.
