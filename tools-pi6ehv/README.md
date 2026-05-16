# PI6EHV Odroid deployment

Deze map bevat de omgeving-specifieke scripts en webpagina voor de PI6EHV
Odroid/WebSDR repeatercomputer.

## Build op de Odroid

Clone de repository op de Odroid en bouw de receiver:

```sh
git clone <repo-url> dvbt-rx
cd dvbt-rx
INSTALL_DEPS=1 tools-pi6ehv/build_odroid_arm64.sh
```

Voor een normale rebuild zonder package-installatie:

```sh
tools-pi6ehv/build_odroid_arm64.sh
```

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

Het startscript gebruikt standaard UDP poort `10000` als MPEG-TS transport van
`rbdvbt_rx` naar ffmpeg. Daarmee wordt de stdout/FIFO-route vermeden; die kan
bij beschadigde of nog niet complete H.264-startpakketten blijven hangen met
meldingen zoals `non-existing PPS 0 referenced` en `no frame`.

De standaard route is:

```text
rbdvbt_rx --udp-ts 127.0.0.1:10000 -> ffmpeg udp://@:10000 -> SRT
```

Voor een andere interne UDP-bestemming:

```sh
UDP_TS=127.0.0.1:10000 tools-pi6ehv/dvbt_rx_pi6ehv.sh
```

De oude naam `UDP_OUT` blijft als alias werken, maar `UDP_TS` is de
voorkeursnaam. Zet `UDP_TS=off` om terug te vallen naar de oude stdout/FIFO-route.

Het script zet ffmpeg standaard op `FFMPEG_MAP=0:v:0`, zodat een onvolledige
of fout gedetecteerde audiostream de SRT MPEG-TS output niet kan blokkeren met
`sample rate not set`. Als audio later betrouwbaar nodig is, kan de map bewust
worden aangepast, bijvoorbeeld met `FFMPEG_MAP=0`.

ffmpeg stderr wordt standaard tijdelijk gelogd in de run-directory onder `/tmp`
en bij cleanup verwijderd. Zet `FFMPEGLOG=/pad/naar/ffmpeg.log` als je die log
voor diagnose wilt bewaren. De watchdog stopt de hele pipeline wanneer ffmpeg
in de recente log herhaald `non-existing PPS` of `no frame!` meldt. Standaard
gebeurt dat bij 20 recente meldingen, zodat de user-service de keten opnieuw
start in plaats van een oude buffer te blijven tonen. Dit kan worden aangepast
met `FFMPEG_NO_FRAME_LIMIT`; zet `FFMPEG_NO_FRAME_LIMIT=0` om deze controle uit
te schakelen.

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

Het startscript bewaakt de receiverstatus. Nadat er eenmaal lock is geweest,
herstart het standaard de RTL-SDR, receiver en ffmpeg pipeline wanneer `locked`
langer dan 15 seconden false blijft, wanneer de status-JSON langer dan 10
seconden niet meer wordt bijgewerkt, of wanneer ffmpeg in een H.264
no-frame/PPS-foutlus blijft hangen. Daarmee valt de SRT-verbinding kort weg in
plaats van dat het laatste ontvangen beeld blijft staan of het handmatig
gestarte script stopt.

```sh
LOCK_LOSS_TIMEOUT=20 STATUS_STALE_TIMEOUT=15 tools-pi6ehv/dvbt_rx_pi6ehv.sh
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
