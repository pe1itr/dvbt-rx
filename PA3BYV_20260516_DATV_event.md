# PA3BYV DATV test event - 2026-05-16

## Samenvatting

Op 2026-05-16 is er getest met station PA3BYV op ongeveer 210 km afstand.
De ontvangst is gelogd met `rbdvbt_rx` op 150 ks en 250 ks. In de logs zijn
meerdere korte transport-stream bursts zichtbaar. De langere en waarschijnlijk
beeld-relevante momenten zitten rond:

- 22:12 bij 150 ks
- 22:29 bij 250 ks
- 22:49 tot 22:55 bij 150 ks

De decoder leverde in deze test soms al TS vanaf ongeveer 4.7-5.1 dB SNR.
De langere of schonere TS-perioden zaten meestal rond 5.3-6.6 dB gemiddeld,
met pieken tot ongeveer 8.7 dB.

PA3BYV gaf tijdens/na de test aanvullende zenderkant-context:

- Bij 150 ks zag PA3BYV zelf dat zijn bitstream richting de zender iets te hoog
  was en hij was niet zeker van de kwaliteit.
- Bij 250 ks gaf PA3BYV aan dat alles aan zenderkant in orde was.

Daarom moeten 150 ks artefacten, continuity errors of beperkte beeldkwaliteit
niet uitsluitend aan de ontvanger worden toegeschreven. Een deel kan aan de
bron-/zenderketen hebben gelegen. Het 250 ks event is voor receiveranalyse
zuiverder qua zenderkant, maar is in replay nog lastiger exact te kalibreren
door de live `iq_drops`.

## Bewijsmateriaal

Er is een screenshot beschikbaar met zichtbaar PA3BYV-beeld:

- screenshot-bestand/titel: `Schermafdruk van 2026-05-16 22-31-20.png`
- inhoud: groot wit `PA3BYV` beeld in het video/player-venster
- context van de operator: dit was net na het moment dat 250 ks was gedecodeerd

Dit screenshot ondersteunt het 250 ks event in de receiverlogs. De log toont de
belangrijkste 250 ks TS-burst eerder rond 22:29:11-22:29:19 in
`rx_20260516_222622.log`, met `recordings/linrad_20260516_222622_s16.iq` als
bijbehorende IQ-opname. Tijdens die burst werden 834 TS-packets gezien bij
ongeveer 5.06-8.05 dB SNR, gemiddeld ongeveer 6.35 dB.

Let op: het screenshot-tijdstip 22:31:20 is niet exact gelijk aan het
TS-bursttijdstip in de decoderlog. Door buffering in de player/pipeline en de
grote `iq_drops` in de live run lopen wall-clock screenshottijd, log-chunktijd
en sample-offset in de IQ-opname niet noodzakelijk exact gelijk. Voor de
documentatie geldt het screenshot daarom als visueel bewijsmateriaal dat er
beeld was rond het 250 ks event, niet als sample-nauwkeurige timestamp.

Een tweede screenshot toont een ander beeldmoment:

- screenshot-bestand/titel: `Schermafdruk van 2026-05-16 22-55-47.png`
- inhoud: testbeeld met tekst `TEST de PA3BYV`, met zichtbare onderrand-
  verstoring/blokken
- context van de operator: dit is een ander event

Dit screenshot past bij de late 150 ks activiteit in `rx_20260516_225314.log`.
De receiverlog toont daar een langere TS-periode rond 22:55:10-22:55:40 met
510 TS-packets, en vooral een sterkere subperiode rond 22:55:24-22:55:40
waarin PAT/PMT/video/audio PID meerdere keren zichtbaar waren. De SNR tijdens
die TS-regels lag ongeveer tussen 4.61 en 5.85 dB, gemiddeld ongeveer 5.14 dB.

Ook hier geldt dat het screenshot-tijdstip niet sample-nauwkeurig met de
decoderlog hoeft samen te vallen. Het is wel een duidelijke visuele bevestiging
dat het late 150 ks event daadwerkelijk beeld opleverde, zij het met zichtbare
artefacten.

## Gebruikte logs

De analyse is uitgevoerd op de bestanden in `logs/`:

- `rx_20260516_215840.log` tot en met `rx_20260516_225314.log`
- 19 logbestanden totaal
- configuratie in de logs: QPSK, DVB-T 2K, GI 1/32, `stdout=ts-only`
- symbol rates: 150k en 250k

De logs bevatten geen wall-clock timestamp per regel. De tijdstippen hieronder
zijn daarom gereconstrueerd uit:

1. de starttijd in de logbestandsnaam, bijvoorbeeld `rx_20260516_222622.log`
   betekent start rond 22:26:22;
2. de RF-duur per live chunk uit de `[resampler]` regels.

De resolutie is daardoor ongeveer een live chunk:

- 150 ks: ongeveer 1.58 s per chunk
- 250 ks: ongeveer 0.47 s per chunk

## Momenten met TS-output

Een moment is als TS-output geteld wanneer een `[ts]` regel `packets > 0`
meldde. De SNR is de laatst gemeten `snr=` uit de voorafgaande `[dvbt2k]`
regel. Bij langere bursts is het bereik en gemiddelde over de TS-regels in
die burst genomen.

| Tijd | SR | SNR bij TS | TS-packets | Interpretatie |
|---|---:|---:|---:|---|
| 22:12:26-22:12:32 | 150k | 5.52-8.73 dB, avg 6.66 | 463 | duidelijke TS, video PID 0x0100 |
| 22:16:42-22:16:43 | 150k | 4.69-5.00 dB, avg 4.85 | 44 | korte TS-burst, video PID 0x0100 |
| 22:18:50-22:18:51 | 150k | 4.96-5.47 dB, avg 5.13 | 119 | korte TS-burst, video PID 0x0100 |
| 22:29:11-22:29:19 | 250k | 5.06-8.05 dB, avg 6.35 | 834 | duidelijke TS, video/audio PID gezien |
| 22:40:45 | 250k | 5.21 dB | 46 | korte TS/SDT, geen PMT/video PID |
| 22:49:41 | 150k | 5.33 dB | 44 | korte TS, geen PMT/video PID |
| 22:49:58-22:49:59 | 150k | 5.14-5.43 dB, avg 5.29 | 158 | TS met video/audio PID |
| 22:51:57-22:52:00 | 150k | 4.76-5.31 dB, avg 5.04 | 136 | TS met video/audio PID |
| 22:52:51 | 150k | 5.30 dB | 135 | TS met video/audio PID |
| 22:53:42-22:53:45 | 150k | 4.68-5.07 dB, avg 4.91 | 95 | TS met video/audio PID |
| 22:55:10-22:55:40 | 150k | 4.61-5.85 dB, avg 5.14 | 510 | langere TS-periode, video/audio PID gezien |

Let op: TS-output betekent dat de receiver MPEG-TS packets heeft geschreven.
Dat is niet automatisch hetzelfde als zichtbaar beeld in de player. Voor beeld
zijn onder andere voldoende aaneengesloten packets, PAT/PMT, video PID en
voldoende weinig continuity errors nodig.

## SNR: 150 ks versus 250 ks

### Alle SNR-metingen

Over alle `dvbt2k snr=` metingen in de logs is het verschil tussen 150 ks en
250 ks klein.

| SR | Aantal metingen | Min | P25 | Gemiddeld | Mediaan | P75 | Max |
|---|---:|---:|---:|---:|---:|---:|---:|
| 150k | 954 | 2.43 | 3.83 | 3.98 | 3.89 | 4.05 | 8.73 |
| 250k | 2289 | 2.89 | 3.87 | 4.01 | 3.92 | 4.03 | 8.30 |

Conclusie voor alle meetpunten: gemiddeld was er praktisch geen SNR-verschil.
Beide symbol rates zaten over de volledige test meestal rond 3.9-4.0 dB.

### Alleen momenten met TS-output

Wanneer alleen de momenten met TS-output worden meegenomen, is er wel een
duidelijk verschil zichtbaar.

| SR | TS-metingen | Min | P25 | Gemiddeld | Mediaan | P75 | Max |
|---|---:|---:|---:|---:|---:|---:|---:|
| 150k | 49 | 4.61 | 4.96 | 5.39 | 5.14 | 5.52 | 8.73 |
| 250k | 16 | 5.06 | 5.42 | 6.28 | 6.07 | 6.85 | 8.05 |

In deze test kwam 250 ks TS-output vooral voor in betere SNR-pieken. Het
gemiddelde bij TS-output lag ongeveer 0.9 dB hoger dan bij 150 ks.

Voorzichtig interpreteren: er zijn meer 150 ks TS-momenten dan 250 ks
TS-momenten, en propagatie/fading kan per run verschillen. De beste conclusie
uit deze logs is:

- 150 ks decodeerde vaker en ook bij iets lagere SNR.
- 250 ks leverde TS vooral wanneer het signaal duidelijk sterker was.
- De praktische TS-drempel lag in deze test rond 4.7-5.1 dB, met betere
  stabiliteit boven ongeveer 5.3 dB.
- De 150 ks beeldkwaliteit kan mede beinvloed zijn door de zenderkant, omdat
  PA3BYV aangaf dat de bitstream naar de zender mogelijk iets te hoog was.
- Het 250 ks beeldmoment is zenderkant waarschijnlijk betrouwbaarder, omdat
  PA3BYV aangaf dat alles daar in orde was.

## Observaties voor decoderverbetering

De logs laten veel periodes zien waar de geschatte SNR rond 3.8-4.1 dB ligt,
maar waar geen TS-output ontstaat. Rond TS-momenten stijgt niet alleen SNR,
maar ook de pilot-lock kwaliteit. Verbeteringen moeten daarom waarschijnlijk
niet alleen naar SNR kijken, maar ook naar lock-continuiteit, timing/frequency
tracking en het vasthouden van outer-state door korte fades.

Kandidaten om verder te onderzoeken:

- minder agressief resetten bij korte low-pilot dips;
- betere continuiteit tussen acquire en track rond de `grdvbt-track reject`
  momenten;
- langere of adaptieve interleaver/outer-state tolerantie tijdens korte fades;
- extra logging die per TS-burst direct wall-clock tijd, SR, SNR, pilot-lock,
  RS-correcties en PID-status op een regel samenvat.

## AFC-observaties

De test is volgens de operator met `--afc` uitgevoerd. Let op: de huidige
`[config]` logregel toont de AFC-instelling niet, dus dit is niet direct uit de
configregel af te lezen. De aanwezigheid van regels met `afc_ok=1` bevestigt
wel dat de AFC-correctielogica actief correcties mocht toestaan, want in de
code wordt `afc_ok` alleen gezet wanneer `cfg->afc_enabled` waar is en de
drift als betrouwbaar genoeg wordt gezien.

In de logs staan de AFC-velden in `[frontend-cont]` regels:

- `bin=a/b`: gevonden carrier-bin versus huidige cursor-bin;
- `afc_delta`: verschil tussen gevonden bin en cursor-bin;
- `afc_ok`: AFC-correctie toegestaan voor deze chunk;
- `afc_count`: aantal chunks waarin dezelfde drifttrend is gezien.

Samenvatting over alle `[frontend-cont]` regels:

| SR | Frontend-cont regels | `afc_delta != 0` | `afc_ok != 0` | `afc_count != 0` |
|---|---:|---:|---:|---:|
| 150k | 145 | 7 | 0 | 0 |
| 250k | 217 | 15 | 2 | 1 |

Bij bijna alle TS-momenten hoefde AFC geen bin-correctie uit te voeren:

- 150k TS-bursts zaten vrijwel steeds op `bin=4/4 afc_delta=0 afc_ok=0`.
- 250k TS-bursts zaten eerst op `bin=3/3 afc_delta=0 afc_ok=0`.
- Later in de 250k log is de vaste bin naar `2/2` verschoven, maar ook daar
  waren de TS-regels zelf `afc_delta=0 afc_ok=0`.

Er zijn twee regels waar `afc_ok=1` zichtbaar is, dus waar AFC een correctie
toestond:

- `rx_20260516_222622.log:3067`, 250k, `bin=2/3 afc_delta=-1 afc_ok=1`,
  CFO ongeveer +69 Hz, lock 0.598.
- `rx_20260516_223955.log:748`, 250k, `bin=2/3 afc_delta=-1 afc_ok=1`,
  CFO ongeveer +69 Hz, lock 0.903.

Deze twee AFC-correcties vallen niet samen met de grootste TS-burst van
22:29:11-22:29:19. Die grote burst liep stabiel zonder zichtbare bin-correctie
op `bin=3/3`.

Interpretatie:

- Voor 150k was de carrier-bin tijdens succesvolle TS meestal stabiel op bin 4.
- Voor 250k was de carrier-bin meestal stabiel op bin 3, met later een sprong
  naar bin 2 in enkele runs.
- De huidige AFC-logica lijkt in deze test zeer conservatief te zijn: ook met
  AFC aan werden vrijwel alle afwijkingen alleen gezien als kandidaat of ruis,
  niet als toegestane correctie.
- De grootste decoderproblemen in deze logs lijken daardoor eerder bij
  lock-continuiteit, pilot-lock en fades te liggen dan bij een continu
  bijregelende AFC.

Voor vervolgtesten is het nuttig om de `[config]` logregel uit te breiden met
de AFC-instelling en per TS-burst de statusvelden `afc_advised`,
`afc_delta_bins`, `afc_trend_count`, `cfo_hz` en `bin_shift` samen te vatten.
Dan kan beter worden vastgesteld of AFC bij 250 ks de sprong tussen bin 3 en
bin 2 helpt opvangen, of juist lock-instabiliteit introduceert.

## Tijd van signaal naar geldige TS

De logs kunnen geen sample-nauwkeurige acquisitietijd geven, maar wel een
praktische chunk-gebaseerde schatting. Hiervoor is gemeten vanaf de eerste
decodeerbare frontend-chunk tot de eerste `[ts] packets > 0` regel.

Een decodeerbare frontend-chunk is hier gedefinieerd als een `[dvbt2k]` regel
die niet als `dropping weak live chunk` is gedropt en waarbij
`avg_pilot_lock >= 0.70`. Dit is de eerste chunk waar de inner chain echt
doorloopt naar Viterbi/outer decode.

De tijdresolutie wordt beperkt door de live chunkgrootte:

- 150 ks met `live_symbols=128`: ongeveer 1.58 s RF per chunk.
- 250 ks met `live_symbols=64`: ongeveer 0.47 s RF per chunk.

Wanneer hieronder `0.00 s` staat, betekent dat niet dat de decoder geen tijd
nodig had. Het betekent dat de eerste TS-output binnen dezelfde live chunk
werd gelogd als de eerste decodeerbare chunk. In de lijnvolgorde is dan te
zien dat na `live chunk done` de Viterbi/outer worker binnen ongeveer
0.1-0.2 s processingtijd TS packets schrijft.

| TS-moment | SR | Eerste decodeerbare chunk | Eerste TS chunk | Chunk-tijd tot TS | Start SNR/lock | TS SNR |
|---|---:|---:|---:|---:|---:|---:|
| 22:12:26 | 150k | 183 | 183 | 0.00 s | 7.25 dB / 0.962 | 7.25 dB |
| 22:16:42 | 150k | 116 | 116 | 0.00 s | 5.00 dB / 0.826 | 5.00 dB |
| 22:18:50 | 150k | 197 | 197 | 0.00 s | 5.47 dB / 0.906 | 5.47 dB |
| 22:29:11 | 250k | 356 | 359 | 1.42 s | 6.58 dB / 0.944 | 8.05 dB |
| 22:40:45 | 250k | 105 | 107 | 0.95 s | 5.12 dB / 0.829 | 5.21 dB |
| 22:49:41 | 150k | 27 | 27 | 0.00 s | 5.33 dB / 0.887 | 5.33 dB |
| 22:49:58 | 150k | 37 | 38 | 1.58 s | 5.00 dB / 0.846 | 5.43 dB |
| 22:51:57 | 150k | 18 | 18 | 0.00 s | 5.31 dB / 0.904 | 5.31 dB |
| 22:52:51 | 150k | 49 | 52 | 4.73 s | 4.79 dB / 0.725 | 5.30 dB |
| 22:53:42 | 150k | 18 | 19 | 1.58 s | 4.91 dB / 0.778 | 5.07 dB |
| 22:55:10 | 150k | 74 | 75 | 1.58 s | 4.85 dB / 0.823 | 4.61 dB |

Over deze TS-bursts:

- mediaan vanaf eerste decodeerbare chunk tot eerste TS: ongeveer 0.95 s;
- gemiddelde: ongeveer 1.08 s;
- maximum in deze logs: ongeveer 4.73 s;
- als alleen niet-nul gevallen worden genomen: gemiddeld ongeveer 1.97 s.

Praktische conclusie: als het signaal meteen sterk genoeg binnenkomt
(`avg_pilot_lock` ruim boven 0.70 en SNR rond 5 dB of hoger), kan de decoder
binnen een of enkele live chunks geldige TS leveren. Voor 150 ks is dat meestal
0-1.6 s op chunkbasis; voor de duidelijke 250 ks burst was dat ongeveer
1.4 s. Zwakkere of net-aan stukken kunnen meerdere chunks nodig hebben voordat
outer sync en RS-cadence goed staan.

## Fading en state-retentie

De logs laten duidelijk zien dat de ontvangst niet continu was. Tijdens de
bruikbare pieken zitten er korte dips in pilot-lock en SNR. De decoder ging
daar niet altijd volledig opnieuw zoeken; op meerdere momenten werd de
bestaande timing/bin/outer-state vastgehouden en kwam er daarna nog TS uit.

### Frontend: niet steeds opnieuw beginnen

Rond de TS-bursts komen vaak regels voor als:

```text
[sync] grdvbt-track reject ... falling back to acquire
[frontend-cont] mode=acquire ... continuous=1 ... bin=4/4 ... lock=...
```

Dat betekent: de track-check was niet sterk genoeg, maar de opnieuw gevonden
positie lag nog binnen de verwachte timing. De frontend schakelde dan naar
`acquire`, maar hield de stream-continuiteit, sequence en carrier-bin vast.
Dit is precies het gewenste gedrag voor korte fading: niet blind resetten als
de nieuwe match nog binnen de timing past.

Voorbeelden:

- `rx_20260516_220739.log` rond 22:12:32: `grdvbt-track reject`, daarna
  `continuous=1`, `bin=4/4`, `outer_state=LOCKED`, en TS blijft doorlopen.
- `rx_20260516_225314.log` rond 22:55:31-22:55:40: meerdere acquire-regels
  met `continuous=1`, `bin=4/4`, en nog steeds TS packets.
- `rx_20260516_222622.log` rond 22:29:11-22:29:19: de 250 ks burst bleef
  grotendeels stabiel op `bin=3/3`, met `continuous=1` en veel geldige TS.

### Low-pilot dips binnen een succesvolle burst

De health-regels laten zien dat TS-output soms kwam in vensters waar ook
low-pilot chunks voorkwamen:

| Moment | SR | Health-observatie | Betekenis |
|---|---:|---|---|
| 22:12 | 150k | `low_pilot=3`, `cont_bad=0`, `outer_state=LOCKED`, `packets=106` gevolgd door meer TS | korte fades werden overleefd |
| 22:18 | 150k | `low_pilot=3`, `outer_state=LOCKED`, `packets=69` | lock bleef bruikbaar ondanks dips |
| 22:29 | 250k | `low_pilot=2`, `outer_state=LOCKED`, `packets=670` | duidelijke 250 ks burst overleefde fades |
| 22:52 | 150k | `low_pilot=1`, `outer_state=LOCKED`, `packets=135` | state-retentie hielp na eerdere slechte chunks |

Dit is een sterke aanwijzing dat het eerdere beleid om niet meteen alles weg
te gooien bij korte dips heeft geholpen. Zonder state-retentie zouden deze
bursts waarschijnlijk vaker opnieuw door acquisitie, deinterleaver-fase en
RS-cadence moeten lopen, waardoor korte beeldmomenten gemist worden.

### Outer decoder: doorzetten in DEGRADED

Ook de outer decoder houdt state vast bij matige RS-resultaten. In plaats van
direct te resetten, gaat hij naar `DEGRADED` en telt fail jobs. Voor 150 ks is
de zachte limiet 6 jobs; voor 250 ks is die 5 jobs. Een harde cadence failure
reset na 3 hard fails.

Dit gedrag is zichtbaar in de bursts:

- Rond 22:12 bij 150 ks verschijnen `degraded` regels met `fail_jobs=1/6` en
  `2/6`, maar er worden nog TS packets geschreven en later is de health-regel
  weer `outer_state=LOCKED`.
- Rond 22:55 bij 150 ks blijft er veel TS komen terwijl `outer_state=DEGRADED`
  staat. De decoder schrijft dus nog door zolang er nog voldoende RS/TS-output
  is.

### Waar het niet genoeg was

De logs tonen ook de grens van deze aanpak. Als de fade of cadence-fout te lang
duurt, reset de outer cadence alsnog:

- `rx_20260516_225314.log:912`: `consecutive_fail_jobs=6; resetting outer cadence`
- `rx_20260516_225314.log:1012`: opnieuw `consecutive_fail_jobs=6`
- `rx_20260516_225131.log:496`: `hard_cadence_fail jobs=3 ... resetting outer cadence`

Dat lijkt terecht: daar waren meerdere opeenvolgende jobs met te weinig RS/TS
kwaliteit. In die situaties zou blijven vasthouden waarschijnlijk vooral
verkeerde deinterleaver/RS-cadence blijven gebruiken.

### Conclusie voor deze test

De logs ondersteunen dat state-retentie heeft geholpen. Succesvolle TS-bursts
vallen samen met:

- stabiele carrier-bin (`bin=4/4` bij 150 ks, `bin=3/3` of later `2/2` bij
  250 ks);
- `continuous=1` ondanks incidentele `grdvbt-track reject`;
- health-vensters met low-pilot dips maar toch `LOCKED` of bruikbare
  `DEGRADED` output;
- outer fail counters die enkele slechte jobs tolereren voordat er gereset
  wordt.

Voor verdere verbetering lijkt de belangrijkste vraag niet of we altijd langer
moeten vasthouden, maar wanneer. Bij korte fades helpt vasthouden duidelijk.
Bij meerdere opeenvolgende hard fails is resetten nog steeds nodig. Een goede
volgende stap is per burst expliciet loggen hoeveel chunks sinds de laatste
goede pilot/RS/TS zijn verstreken, zodat de hold-tijd beter af te regelen is
voor 150 ks en 250 ks.

## Momenten waar low-pilot hold waarschijnlijk had geholpen

De huidige frontend reset de inner continuiteit bij elke regel van deze vorm:

```text
[dvbt2k] dropping weak live chunk ...; resetting inner continuity
[fifo1] reset ... reason=low-pilot
```

De outer decoder heeft al beleid om korte low-pilot periodes te tolereren,
maar de frontend/fifo reset nog direct. In de logs zijn meerdere momenten te
vinden waar zo'n low-pilot reset kort na geldige TS-output of tussen twee
TS-bursts viel. Dat zijn de beste kandidaten waar "niet direct resetten maar
even vasthouden" had kunnen helpen.

| Tijd | SR | Low-pilot periode | Context | Waarom relevant |
|---|---:|---|---|---|
| 22:12:33.9-22:12:44.9 | 150k | 6 drops, lock 0.137-0.567, SNR 3.85-4.64 dB | direct na sterke TS-burst 22:12:26-22:12:32, health ervoor `outer_state=LOCKED`, 357 packets in venster | waarschijnlijk had hold de bestaande cadence langer kunnen bewaren, maar het signaal zakte daarna duidelijk weg |
| 22:16:45.5-22:16:56.5 | 150k | 8 drops, lock 0.065-0.565, SNR 3.80-4.34 dB | direct na korte geldige TS op 22:16:42-22:16:43 | mogelijk nuttig, maar de fade werd vrij diep |
| 22:18:53.2-22:19:04.3 | 150k | 8 drops, lock 0.062-0.609, SNR 3.81-4.43 dB | direct na TS op 22:18:50-22:18:51, health ervoor `outer_state=LOCKED` | goede kandidaat voor hold; eerste drop zat nog op lock 0.609 |
| 22:29:17.0-22:29:18.0 | 250k | 3 drops, lock 0.072-0.498, SNR 4.03-4.57 dB | midden in de 250k piek: TS tot 22:29:16.6 en opnieuw TS op 22:29:19.4 | sterkste bewijs: low-pilot reset zat tussen twee TS-momenten; hold had deze onderbreking mogelijk kunnen overbruggen |
| 22:29:20.4-22:29:23.7 | 250k | 7 drops, lock 0.070-0.686, SNR 3.97-4.77 dB | vlak na 250k TS op 22:29:19.9 | mogelijk nuttig; eerste drop zat bijna op de decodegrens met lock 0.674 |
| 22:49:42.6-22:49:53.6 | 150k | 7 drops, lock 0.242-0.690, SNR 4.00-4.52 dB | na TS op 22:49:41 en voor nieuwe TS op 22:49:58 | goede kandidaat; eerste drop zat met lock 0.690 net onder `decode_min=0.70` |
| 22:50:03.1-22:50:12.5 | 150k | 7 drops, lock 0.072-0.570, SNR 3.83-4.49 dB | direct na TS op 22:49:58-22:49:59 | mogelijk nuttig, maar daarna geen directe TS-herstel in hetzelfde venster |
| 22:52:02.5-22:52:13.6 | 150k | 8 drops, lock 0.064-0.389, SNR 3.82-4.20 dB | na TS op 22:51:57-22:52:00, health `outer_state=DEGRADED` | hold zou cadence kunnen bewaren, maar de fade was diep |
| 22:52:53.0-22:53:04.0 | 150k | 8 drops, lock 0.064-0.375, SNR 3.84-4.16 dB | direct na TS op 22:52:51 | mogelijk nuttig voor state-retentie, maar weinig kans op directe decode tijdens de dip |
| 22:53:47.1-22:53:58.2 | 150k | 6 drops, lock 0.118-0.697, SNR 3.88-4.56 dB | na TS op 22:53:42-22:53:45, health ervoor `outer_state=LOCKED` | goede kandidaat; eerste drop lock 0.697 zat praktisch op de drempel |
| 22:55:12.3-22:55:17.0 | 150k | 4 drops, lock 0.065-0.346, SNR 3.85-4.06 dB | na korte TS op 22:55:10 en voor grote TS-burst vanaf 22:55:24.9 | mogelijk nuttig als hold de cadence tot de volgende piek had kunnen bewaren |
| 22:55:23.3 | 150k | 1 drop, lock 0.068, SNR 4.82 dB | vlak voor grote TS-burst vanaf 22:55:24.9 | interessant randgeval: SNR was goed maar pilot-lock slecht; niet resetten had mogelijk minder opbouwtijd gegeven |
| 22:55:42.2-22:55:53.3 | 150k | 8 drops, lock 0.068-0.661, SNR 3.95-4.44 dB | direct na de lange 22:55 TS-burst, health `outer_state=DEGRADED` met 292 packets | nuttig voor langer vasthouden na beeld, maar signaal viel daarna weg |

De sterkste kandidaten zijn dus:

- 22:29:17-22:29:18 bij 250k, omdat de low-pilot reset tussen twee TS-momenten
  zat.
- 22:49:42-22:49:53 bij 150k, omdat de eerste drop net onder de lockdrempel
  zat en later opnieuw TS kwam.
- 22:53:47-22:53:58 bij 150k, omdat de drop na `outer_state=LOCKED` kwam en
  de eerste lockwaarde 0.697 was, praktisch op de huidige drempel 0.70.
- 22:55:23 bij 150k, omdat er vlak daarna een grote TS-burst kwam.

Deze punten ondersteunen een concrete wijziging: bij bestaande frontend/outer
lock niet direct `fifo1 reset reason=low-pilot` doen bij een enkele chunk onder
`decode_min`. In plaats daarvan zou de decoder bijvoorbeeld 1-3 chunks kunnen
holden, afhankelijk van SR en recente TS/RS-kwaliteit. Voor 150 ks is 1 chunk
ongeveer 1.58 s; voor 250 ks is 1 chunk ongeveer 0.47 s.

## IQ replay targets voor optimalisatie

De map `recordings/` bevat Linrad s16 IQ-opnamen die vrijwel een-op-een
corresponderen met de `rx_20260516_*` logs. Daardoor kunnen de interessante
TS- en fading-events reproduceerbaar opnieuw door de decoder gehaald worden.

De bestanden zijn interleaved signed 16-bit IQ (`--input-format s16`) met
sample-rate 1010526 Hz. De bestandsduur is berekend als:

```text
duur = bytes / (4 * 1010526)
```

### Belangrijkste replay-opnamen

| Opname | SR in log | Duur | Relevante offsets | Waarom deze opname |
|---|---:|---:|---|---|
| `recordings/linrad_20260516_220739_s16.iq` | 150k | 359.8 s | TS 287-293 s; low-pilot 294.9-305.9 s | sterke 150k TS-burst, daarna fade |
| `recordings/linrad_20260516_221341_s16.iq` | 150k | 434.0 s | TS 181-182 s; TS 309-310 s; fades er direct achter | korte 150k beeld/TS-momenten |
| `recordings/linrad_20260516_222622_s16.iq` | 250k | 347.6 s | TS 169-177 s; low-pilot tussen TS 175-176 s; fade 178.4-181.7 s | belangrijkste 250k event; beste test voor low-pilot hold |
| `recordings/linrad_20260516_224900_s16.iq` | 150k | 150.4 s | TS 41 s; fade 42.6-53.6 s; TS 58-59 s | fade tussen twee 150k TS-momenten |
| `recordings/linrad_20260516_225131_s16.iq` | 150k | 101.3 s | TS 26-29 s; TS 80 s; fades erachter | twee korte 150k TS-momenten |
| `recordings/linrad_20260516_225314_s16.iq` | 150k | 226.0 s | TS 28-31 s; pre-burst drop 129.3 s; grote TS 130.9-146.7 s; fade 148.2-159.3 s | beste 150k late-event opname; grote burst plus fade |
| `recordings/linrad_20260516_223955_s16.iq` | 250k | 121.5 s | korte TS rond 50 s; fade 51.1-54.4 s | korte 250k TS/SDT-only case |

### Eerste optimalisatie-set

Voor codewijzigingen rond low-pilot hold is een kleine vaste regressieset
handig:

1. `linrad_20260516_222622_s16.iq`, offset 165-183 s, SR 250k.
   Dit bevat de belangrijkste 250k TS-burst en de low-pilot dip tussen twee
   TS-momenten.
2. `linrad_20260516_224900_s16.iq`, offset 38-62 s, SR 150k.
   Dit bevat een fade tussen twee 150k TS-momenten.
3. `linrad_20260516_225314_s16.iq`, offset 126-150 s, SR 150k.
   Dit bevat de pre-burst low-pilot drop en daarna de grote 150k TS-burst.
4. `linrad_20260516_220739_s16.iq`, offset 284-307 s, SR 150k.
   Dit bevat een sterke lock, TS-output en daarna wegzakkend signaal.

Bij replay moeten we dezelfde kernparameters gebruiken als in de logs:

```sh
build/rbdvbt_rx \
  --stdin \
  --input-format s16 \
  --sample-rate 1010526 \
  --sr 150k \
  --gi 1/32 \
  --fec 1/2 \
  --live \
  --live-symbols 128 \
  --afc \
  --ts-out /tmp/replay.ts \
  --loglevel info
```

Voor 250k:

```sh
build/rbdvbt_rx \
  --stdin \
  --input-format s16 \
  --sample-rate 1010526 \
  --sr 250k \
  --gi 1/32 \
  --fec 1/2 \
  --live \
  --live-symbols 64 \
  --afc \
  --ts-out /tmp/replay.ts \
  --loglevel info
```

Voor korte segment-replay kan het IQ-bestand eerst op byte-offset worden
gesneden. Bij s16 complex IQ is 1 sample 4 bytes. Voor offset `T` seconden:

```text
skip_bytes = T * 1010526 * 4
max_samples = segment_seconds * 1010526
```

Een praktisch replay-script of tool zou de offsets hierboven kunnen gebruiken
om automatisch segmenten te draaien en per run deze metrics te vergelijken:

- eerste TS-tijd;
- totaal aantal TS-packets;
- PAT/PMT/video PID gezien;
- continuity errors;
- RS corrected/uncorrectable;
- aantal `low-pilot` resets;
- aantal `frontend-discontinuity` resets;
- aantal outer `relock` events;
- langste aaneengesloten TS-burst.

Daarmee kunnen we low-pilot hold objectief tunen: een wijziging is nuttig als
de TS-bursts langer worden of sneller terugkomen zonder dat RS/CC-fouten
duidelijk verslechteren.

### Voor/na-meting

Voor reproduceerbare metingen is `tools/pa3byv_replay_metrics.py` toegevoegd.
De tool draait vaste IQ-segmenten door `build/rbdvbt_rx` en telt uit de
receiver-log onder andere:

- `ts_packets_total`
- aantal TS-reports met packets
- PAT/PMT/SDT packets
- continuity errors
- low-pilot drops en low-pilot FIFO-resets
- frontend discontinuity resets
- outer relocks
- max/gemiddelde SNR en pilot-lock

Targets tonen:

```sh
tools/pa3byv_replay_metrics.py --list
```

Baseline meten:

```sh
tools/pa3byv_replay_metrics.py \
  --csv-out /tmp/pa3byv_baseline.csv \
  --json-out /tmp/pa3byv_baseline.json \
  --keep-logs /tmp/pa3byv_baseline_logs
```

Na een decoderwijziging dezelfde meting opnieuw:

```sh
tools/pa3byv_replay_metrics.py \
  --csv-out /tmp/pa3byv_after.csv \
  --json-out /tmp/pa3byv_after.json \
  --keep-logs /tmp/pa3byv_after_logs
```

De primaire metric voor low-pilot hold is `ts_packets_total`. Secundaire
metrics zijn `cc_errors_total`, `outer_relock`, `fifo_reset_low_pilot` en
`ts_reports_with_packets`. Een verbetering is pas overtuigend als
`ts_packets_total` stijgt zonder duidelijke verslechtering in continuity errors
of onnodige relocks.

Na een replaycontrole is de scherpste primaire metric aangescherpt naar
`ts_file_packets`: het aantal werkelijk geschreven MPEG-TS packets in het
outputbestand, berekend als `ts_file_bytes / 188`. De oudere
`ts_packets_total` uit de `[ts]` logregels blijft nuttig als validator/log-
metric, maar kan afwijken van het aantal bytes dat echt naar `--ts-out` is
geschreven.

Belangrijke nuance: in de live logs stonden soms grote `iq_drops`. Daardoor is
de wall-clock/log-offset niet altijd exact dezelfde sample-offset in de
continue opname. De replay-tool gebruikt de log-offsets als nominale start en
heeft daarom `--offset-shift` om de opname-offset te kalibreren. Als een target
geen TS reproduceert terwijl dat in de log wel gebeurde, moet eerst met
`--offset-shift` rond het nominale punt worden gezocht. De gekozen shift moet
daarna voor baseline en na-meting gelijk blijven.

### Gekalibreerde baseline

Een eerste offsetkalibratie is uitgevoerd met de huidige decoderstand
(`30c2d13 Improve live GUI refresh cadence` als laatste code-commit, plus de
uncommitted replay-tool/documentatie). De 150k targets reproduceren TS en zijn
bruikbaar als voor/na-regressieset.

De gekozen gekalibreerde offsets in `tools/pa3byv_replay_metrics.py` zijn:

| Target | Opname | Offset | Duur | Reden |
|---|---|---:|---:|---|
| `150k_between_ts_fade` | `linrad_20260516_224900_s16.iq` | 38 s | 24 s | nominale offset reproduceert TS |
| `150k_pre_big_burst` | `linrad_20260516_225314_s16.iq` | 121 s | 24 s | beste sweep rond nominale offset: 163 TS-packets |
| `150k_strong_then_fade` | `linrad_20260516_220739_s16.iq` | 274 s | 23 s | beste sweep rond nominale offset: 176 TS-packets |

Baseline-resultaat op basis van logreports:

| Target | SR | TS packets | TS reports | PAT | PMT | CC errors | low-pilot drops | low-pilot resets | outer relock | max SNR |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `150k_between_ts_fade` | 150k | 161 | 3 | 3 | 3 | 4 | 3 | 3 | 0 | 5.41 dB |
| `150k_pre_big_burst` | 150k | 163 | 6 | 9 | 9 | 9 | 1 | 1 | 0 | 5.79 dB |
| `150k_strong_then_fade` | 150k | 176 | 5 | 6 | 6 | 10 | 2 | 2 | 0 | 6.25 dB |

De huidige 250k target `250k_main_fade` reproduceert nog geen TS in replay,
ook niet bij een grove offset-sweep. Rond offset 270 s is wel een hoge SNR-piek
gevonden (`max_snr_db` ongeveer 7.45 dB), maar zonder TS-output. Dit target
moet daarom nog verder gekalibreerd worden voordat het als harde voor/na-meting
gebruikt kan worden. Voor de eerste low-pilot-hold optimalisatie is de 150k set
wel bruikbaar.

Voorbeeld replay van `150k_pre_big_burst` met TS-output naar bestand:

```sh
tools/pa3byv_replay_metrics.py \
  --target 150k_pre_big_burst \
  --ts-out /tmp/pa3byv_150k_pre_big_burst.ts \
  --keep-logs /tmp/pa3byv_150k_pre_big_burst_replay
```

Resultaat:

- `ts_packets_total` uit logreports: 163
- `ts_file_packets` werkelijk geschreven naar TS-bestand: 137
- TS-bestand: `/tmp/pa3byv_150k_pre_big_burst.ts`
- bestandsgrootte: 25756 bytes, exact 137 * 188 bytes
- PAT/PMT gezien: 9/9 in de logreports
- continuity errors: 9
- low-pilot drops/resets: 1/1

Voor voor/na-vergelijking gebruiken we vanaf nu primair `ts_file_packets`.

### Eerste optimalisatieproef: low-pilot hold

Geteste wijziging: bij een bestaande live lock een low-pilot chunk niet direct
de inner/outer continuiteit laten resetten als de pilot lock nog bruikbaar is
voor metadata (`avg_pilot_lock >= 0.45`). Hele zwakke chunks en pre-lock
situaties blijven resetten. De hold is bewust beperkt tot 1 chunk.

Before/after met dezelfde replaytargets:

| Target | SR | Before `ts_file_packets` | After `ts_file_packets` | Effect |
|---|---:|---:|---:|---|
| `150k_between_ts_fade` | 150k | 68 | 68 | geen extra TS; 1 reset is vervangen door hold |
| `150k_pre_big_burst` | 150k | 137 | 137 | geen effect; low-pilot reset zat voor bestaande lock |
| `150k_strong_then_fade` | 150k | 152 | 152 | geen effect; fade werd te diep/discontinu |
| `250k_main_fade` | 250k | 0 | 0 | target reproduceert nog geen TS |

Belangrijkste logbewijs bij `150k_between_ts_fade`:

- before: na TS-output kwam `avg_pilot_lock=0.49150`, daarna direct
  `resetting inner continuity` en `fifo1 reset reason=low-pilot`.
- after: dezelfde chunk wordt
  `holding weak live chunk ... preserving inner continuity`.
- er kwamen in deze replay geen extra TS-packets bij, omdat de capture direct
  na deze fade eindigt. De wijziging is dus logisch correct voor live gedrag,
  maar nog niet bewezen als packetwinst op deze korte replay.

Voor `150k_pre_big_burst`, het event met 137 echte TS-packets, is de bottleneck
niet de low-pilot reset. In dat segment valt de enige low-pilot reset voor de
bestaande lock. Daarna blijft de frontend continu, maar de outer decoder loopt
door meerdere RS/continuity-fouten naar `outer_state=degraded`. De meest
waarschijnlijke verbeterpunten voor meer packets in dit event liggen daarom in:

- robuustere outer/deinterleaver state-retentie tijdens korte gedegradeerde
  periodes;
- beter omgaan met slechte RS-jobs zonder te vroeg bruikbare streamstate kwijt
  te raken;
- extra calibratie van de replay-offsets, vooral voor het 250k target dat nog
  geen TS reproduceert;
- validatie of de 150k bronbitstream zelf fouten bevatte, omdat PA3BYV aangaf
  dat zijn bitstream naar de zender toen mogelijk iets te hoog/kwalitatief
  onzeker was.

Controlemetingen op `150k_pre_big_burst`:

| Variant | `ts_file_packets` | Opmerking |
|---|---:|---|
| baseline / AFC aan | 137 | huidige beste replay |
| AFC uit | 137 | geen verschil |
| FEC 2/3 | 0 | fout voor dit event |
| FEC 3/4 | 0 | fout voor dit event |

### Tweede optimalisatieproef: productive degraded outer jobs

Geteste wijziging: een outer job met veel RS-fouten telt minder hard richting
relock als er nog bruikbare output uit komt (`written > 0`, `rs_ok > 0` of
geldige TS-packets in de validator). Zulke jobs worden nu als
`productive=1` gelogd en verhogen de normale fail-counter pas na twee
opeenvolgende productive degraded jobs. Complete cadence failures
(`rs_ok=0`, `written=0`) blijven hard meetellen.

Replay na deze wijziging, inclusief de eerdere low-pilot hold:

| Target | SR | `ts_file_packets` | `outer_degraded` | `outer_productive_degraded` | `outer_relock` | Effect |
|---|---:|---:|---:|---:|---:|---|
| `150k_between_ts_fade` | 150k | 68 | 0 | 0 | 0 | geen extra TS |
| `150k_pre_big_burst` | 150k | 137 | 5 | 4 | 0 | state-retentie verbeterd, maar geen extra TS |
| `150k_strong_then_fade` | 150k | 152 | 2 | 2 | 0 | state-retentie verbeterd, maar geen extra TS |
| `250k_main_fade` | 250k | 0 | 0 | 0 | 0 | target reproduceert nog geen TS |

Logbewijs bij `150k_pre_big_burst`: vier degraded jobs waren nog productief.
Voorbeelden:

- `rs_ok=26 rs_uncorrectable=34 written=22 productive=1`
- `rs_ok=1 rs_uncorrectable=58 written=1 productive=1`
- `rs_ok=9 rs_uncorrectable=50 written=9 productive=1`
- `rs_ok=11 rs_uncorrectable=48 written=9 productive=1`

De oude code zou deze reeks als fail_jobs 1, 2, 3, 4, 5 hebben geteld. De
nieuwe code houdt dit op fail_jobs 0, 0, 1, 2 en pas 3 bij de eerste volledige
niet-productieve job. Dat is robuuster voor live fading, maar in deze korte
replay leverde het geen extra packets op omdat er ook in de oude situatie nog
geen relock/reset werd bereikt.

Conclusie na twee optimalisatieproeven: de huidige korte replaytargets tonen
wel betere state-retentie, maar nog geen hogere `ts_file_packets`. Voor echte
packetwinst is de volgende kandidaat een beperkte local re-scan rond de
bestaande outer alignment (`deint_phase`, `rs_phase`, `block_phase`) wanneer
een degraded job niet-productief wordt of wanneer meerdere productive degraded
jobs elkaar opvolgen.

### Derde optimalisatieproef: local outer realign

Geteste wijziging: bij een niet-productieve degraded outer job
(`rs_ok=0`, `written=0`) wordt dezelfde inner chunk nog een keer gescand voor
een lokaal nieuw outer alignment. Dit gebeurt alleen nadat de normale cadence
geen output opleverde, zodat er geen TS-packets gedupliceerd worden. Een local
realign mag alleen overnemen als de kandidaat minstens 1 RS-clean blok heeft;
een puur sync-only kandidaat wordt gelogd maar niet gebruikt.

Replay na deze wijziging:

| Target | SR | `ts_file_packets` | local attempts | local acquired | Effect |
|---|---:|---:|---:|---:|---|
| `150k_between_ts_fade` | 150k | 68 | 0 | 0 | geen kandidaat |
| `150k_pre_big_burst` | 150k | 137 | 1 | 0 | kandidaat had sync, maar geen RS-clean blok |
| `150k_strong_then_fade` | 150k | 152 | 0 | 0 | geen niet-productieve degraded job in replay |
| `250k_main_fade` | 250k | 0 | 0 | 0 | target reproduceert nog geen TS |

Logbewijs bij `150k_pre_big_burst`:

- poging na niet-productieve degraded job:
  `blocks=60 rs_uncorrectable=60 rs_buffer=37`
- beste local scan:
  `rs_ok=0/1 sync_ok=40/8 sync_only=32 blocks=48 reason=no-rs-clean`

Conclusie: de local re-scan ziet wel reststructuur in de fade (`sync_ok=40`),
maar er is geen enkel RS-clean blok. Een agressieve sync-only overname is
getest en schreef ook 0 TS-packets; dat zou live zelfs riskant zijn omdat de
outer cadence naar een alignment zonder RS-bevestiging kan springen. Daarom is
de veilige variant aangehouden: wel proberen en loggen, alleen overnemen met
RS-bevestiging.
