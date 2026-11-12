# PI6EHV Odroid deployment

Deze map bevat de omgeving-specifieke scripts en webpagina voor de PI6EHV
Odroid/WebSDR repeatercomputer.

## Build op de Odroid

Clone de repository op de Odroid en bouw de receiver:

```sh
git clone <repo-url> dvbt-rx
cd dvbt-rx
INSTALL_DEPS=1 RUN_TESTS=1 tools-pi6ehv/build_odroid_arm64.sh
```

Voor een normale rebuild zonder package-installatie:

```sh
tools-pi6ehv/build_odroid_arm64.sh
```

De build kiest `Release`, twee parallelle taken en geen Qt6/X11. Op ARM64
wordt de decoder afgestemd op de N2 A73/A53-cores met `-march=armv8-a` en
`-mtune=cortex-a73.cortex-a53`, gelijk aan `linrad_dvb`. Hiervoor is een
64-bit OS en een GCC-versie met deze tuning nodig; CMake controleert dat.
Op x86 kiest het script automatisch `generic`. `TARGET_CPU=generic` geeft
ook op ARM64 een build zonder N2-afstemming.

Opties: `JOBS=N`, `WITH_X11=1` voor decoderdiagnostiek en `RUN_TESTS=1` voor
CTest na de build. Python 3 hoort bij de testafhankelijkheden. De tests
gebruiken geen SDR-hardware. Op ARM64 wordt ook de upstream NEON-kerneltest
gebouwd en uitgevoerd; streamingtests draaien met SIMD, geforceerd scalar en
een build zonder expliciete SIMD. NEON zelf zat al in de gedeelde decoder.

Voor een ontvangstvergelijking:

```sh
LOGLEVEL=info tools-pi6ehv/dvbt_rx_pi6ehv.sh
# Stop de eerste run voordat je met dezelfde instellingen scalar vergelijkt:
LINRAD_DVBT_VITERBI_SCALAR=1 LOGLEVEL=info tools-pi6ehv/dvbt_rx_pi6ehv.sh
```

Zoek naar `kernel=neon-float` respectievelijk `kernel=scalar-float`. Vergelijk
ook `iq_drops`, `fifo_drops`, TS/RS-resultaten en de totale CPU-belasting met
SRT actief. Meer threads zijn geen garantie op snellere ontvangst. De native
N2-throughput en 1080p-encodingcapaciteit moeten op de doelcomputer worden
gemeten; de desktoptests bewijzen dat niet.

## Webstatus installeren

De ontvanger schrijft standaard status naar:

```text
/var/www/html/dvb/dvbt-rx-status.json
```

Plaats de HTML-statuspagina in de webroot:

```sh
sudo mkdir -p /var/www/html/dvb
sudo cp tools-pi6ehv/dvbt-rx-status.html /var/www/html/dvbt-rx-status.html
```

Zorg dat de gebruiker die `rbdvbt_rx` draait mag schrijven in
`/var/www/html/dvb`.
De webpagina toont `Ontvanger offline` wanneer het JSON-bestand ontbreekt of
wanneer de `updated_unix` timestamp meer dan 10 seconden oud is.

## Ontvanger starten

```sh
tools-pi6ehv/dvbt_rx_pi6ehv.sh
```

Belangrijke defaults:

```text
RF:          436000000 Hz
RTL-SDR:     00000001
Sample rate: 1010526
Symbolrate: 333000
Gain:        30
GI:          1/32
FEC:         2/3
Status JSON: /var/www/html/dvb/dvbt-rx-status.json
SRT:         srt://44.137.26.85:4001?mode=caller&latency=500000
Loglevel:    quiet
AFC:         aan
```

Standaard gebruikt het script de PI6EHV ffmpeg met SRT support naast de repo:

```text
../ffmpeg/ffmpeg
```

Deze ffmpeg moet SRT ondersteunen. Controleer dat met:

```sh
../ffmpeg/ffmpeg -hide_banner -protocols | grep '^  srt$'
```

Voor een andere ffmpeg binary:

```sh
FFMPEG=/pad/naar/ffmpeg tools-pi6ehv/dvbt_rx_pi6ehv.sh
```

Het startscript gebruikt dezelfde SRT-bewaker als `linrad_dvb`. De route is:

```text
rbdvbt_rx --udp-ts 127.0.0.1:10000 -> rbdvbt_srt -> ffmpeg -> SRT
```

Zonder video blijft de ontvanger actief en is SRT uit. Videopayload start de
SRT-caller; 2000 ms zonder nieuwe videopayload stopt alleen FFmpeg/SRT.
Nieuwe ontvangst start de uitgang automatisch opnieuw. Uitgangsfouten worden
elke 2000 ms opnieuw geprobeerd zolang er video binnenkomt. Er is hiervoor
geen herstart van RTL-SDR of decoder nodig.

De uitvoer is dezelfde als in `linrad_dvb`: 1920×1080p25 H.264 op
4000 kbit/s, preset `veryfast`, optionele AAC-audio op 128 kbit/s,
48 kHz stereo. Deïnterlacing, correcte beeldverhouding bij anamorfische
bronnen, vierkante pixels en zwarte balken zijn inbegrepen.

De instellingen corresponderen met `[srt]` in `linrad-dvb.ini`:

| Environment | INI-sleutel | Default |
|---|---|---|
| `SRT_URL` | `url` | zie boven |
| `SRT_LOSS_TIMEOUT_MS` | `loss_timeout_ms` | `2000` |
| `SRT_RETRY_INTERVAL_MS` | `retry_interval_ms` | `2000` |
| `SRT_FPS` | `fps` | `25` |
| `SRT_VIDEO_BITRATE_KBPS` | `video_bitrate_kbps` | `4000` |
| `SRT_X264_PRESET` | `x264_preset` | `veryfast` |
| `SRT_VIDEO_THREADS` | `video_threads` | `0` (auto) |
| `SRT_FILTER_THREADS` | `filter_threads` | `0` (auto) |

Met `SRT_PROFILE=auto` (default) wordt een ODROID-N2/N2+ via het boardmodel
herkend en automatisch op `ultrafast` / drie videothreads / één filterthread
ingesteld, gelijk aan het upstream N2-profiel. Expliciet kiezen:

```sh
SRT_PROFILE=odroid-n2 tools-pi6ehv/dvbt_rx_pi6ehv.sh
```

`SRT_PROFILE=generic` kiest de algemene defaults uit de tabel. Expliciete
`SRT_X264_PRESET`, `SRT_VIDEO_THREADS` en `SRT_FILTER_THREADS` gaan voor het
profiel. Threads gelden per pool, niet als totaal CPU-budget.

De resolutie is vast 1920×1080. De oude `FFMPEG_SCALE`, `FFMPEG_MAP`,
`FFMPEG_AUDIO` en `FFMPEG_VIDEO_*`-opties zijn vervangen door deze route en
`SRT_*`-instellingen.

`UDP_TS=127.0.0.1:10000` kiest de lokale TS-ingang; `UDP_OUT` blijft een alias.
Gebruik een numeriek lokaal IPv4-adres. `UDP_TS=off` kiest de stdout/FIFO-route.
De bewaker is ook los te starten bij een al draaiende decoder met UDP-uitgang:

```sh
./build/rbdvbt_srt --udp-input 127.0.0.1:10000 \
  --url 'srt://44.137.26.85:4001?mode=caller&latency=500000' \
  --ffmpeg ../ffmpeg/ffmpeg
```

SRT/FFmpeg stderr wordt tijdelijk gelogd onder `/tmp` en bij cleanup verwijderd.
Zet `FFMPEGLOG=/pad/naar/ffmpeg.log` om die log te bewaren.

Voor tijdelijke diagnose kan het receiver-loglevel worden verhoogd:

```sh
LOGLEVEL=info tools-pi6ehv/dvbt_rx_pi6ehv.sh
```

Receiverlogs worden in `logs/` bewaard, maar het script houdt standaard alleen
de laatste 10 `rx_*.log` bestanden. Pas dit aan met `LOG_KEEP=N`.

Het PI6EHV-startscript zet live AFC standaard aan, zodat de ontvanger kleine
carrier-bin drift kan volgen. Voor een vergelijkende test kan dit uit:

```sh
AFC=0 tools-pi6ehv/dvbt_rx_pi6ehv.sh
```

De standaard live chunkgrootte is 128 OFDM-symbols. Kleinere chunks geven
snellere statusupdates, maar maken de reduced-bandwidth live-keten gevoeliger
voor korte trackingdippen.

Het startscript bewaakt de receiverstatus. Nadat er eenmaal lock of OFDM-sync
is geweest, herstart het de pipeline alleen als status JSON langer dan
`STATUS_STALE_TIMEOUT` (standaard 30 seconden) ontbreekt of niet wordt bijgewerkt.
Ontvangstverlies wordt door de SRT-bewaker afgehandeld en herstart de ontvanger
niet meer. De oude `LOCK_LOSS_TIMEOUT` en `FFMPEG_NO_FRAME_LIMIT` vervallen.

```sh
STATUS_STALE_TIMEOUT=15 tools-pi6ehv/dvbt_rx_pi6ehv.sh
```

Watchdog-herstarts zijn standaard onbeperkt. Gebruik `MAX_RESTARTS=N` om dat te
begrenzen, `RESTART_DELAY=N` voor de pauze tussen pogingen, of
`RESTART_ON_WATCHDOG=0` om terug te vallen naar stoppen en de systemd
`Restart=on-failure` policy te laten herstarten.

## User systemd service

Het bestand `dvbt-rx.service` is bedoeld voor een user-service en gaat uit van
een clone in `~/dvbt-rx`.
De service start niet wanneer `nicam-rx.service` actief is, omdat beide services
dezelfde RTL-SDR ontvanger gebruiken.

```sh
mkdir -p ~/.config/systemd/user
cp tools-pi6ehv/dvbt-rx.service ~/.config/systemd/user/dvbt-rx.service
systemctl --user daemon-reload
systemctl --user enable --now dvbt-rx.service
```

Status en logs:

```sh
systemctl --user status dvbt-rx.service
journalctl --user -u dvbt-rx.service -f
```
