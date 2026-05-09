# Architectuur

Dit document beschrijft de huidige opzet van `rbdvbt_rx` en legt de
belangrijkste doorbraakmomenten vast. Het doel is praktisch: bij een volgende
IQ-opname sneller van ruwe samples naar een afspeelbare MPEG-TS komen.

## Doel

`rbdvbt_rx` is een kleine reduced-bandwidth DVB-T ontvanger voor
amateur-DATV-experimenten. De primaire flow is:

```text
IQ opname of SDR stream
  -> rbdvbt_rx
  -> MPEG-TS
  -> ffplay/VLC/analyse tooling
```

Belangrijke ontwerpregels:

- `stdout` bevat alleen MPEG-TS wanneer `--stdout-ts` of `--ts-out -` wordt
  gebruikt.
- Alle logging en diagnostiek gaat naar `stderr`.
- DVB-T parameters worden voorlopig expliciet gekozen via de CLI; TPS-autodetectie
  is nog geen onderdeel van de lock-keten.
- De receiver is bewust smal: QPSK, DVB-T 2K, bekende reduced-bandwidth modes,
  sterke instrumentatie.

## Actieve binaries

De actieve software zit in `src/`, `include/` en `tools/`.

| Binary | Doel |
|---|---|
| `rbdvbt_rx` | Ontvangstketen van IQ naar constellatie, demap, Viterbi, outer FEC en MPEG-TS. |
| `rbdvbt_status_watch` | Terminalmonitor voor de periodieke JSON-status van de receiver. |
| `dvbt_fec_snr_plot` | Simulatietool voor FEC/packet-error grafieken versus C/N. |
| `portsdown_iq_dump` | Hulpmiddel om via de Portsdown DVB-T stack synthetische IQ te maken. Dit target gebruikt `other_parties/portsdown4`. |

## Hoofdmodules

| Bestand | Verantwoordelijkheid |
|---|---|
| `src/main.c` | Minimale entrypoint: parse config en start receiver. |
| `src/config.c`, `include/config.h` | CLI, DVB-T parameters, outputpaden en debugopties. |
| `src/iq_input.c`, `include/iq_input.h` | Leest interleaved IQ (`s16` of `u8`) van `stdin`, converteert naar complex float en verzamelt inputstatistiek. |
| `src/receiver.c`, `include/receiver.h` | Top-level dispatcher. De daadwerkelijke decode loopt nu via de constellation/probe route. |
| `src/probe_constellation.c` | Grootste DSP-deel: resampling, CP timing, FFT, pilot-lock, equalizing, QPSK demap, bitdeinterleaving en Viterbi-input. |
| `src/dvbt_2k_model.c`, `include/dvbt_2k_model.h` | DVB-T 2K carrier model: active carriers, pilots, TPS, data carriers, PRBS en symbol interleaver/deinterleaver. |
| `src/dvbt_outer.c`, `include/dvbt_outer.h` | Outer receiver: byte deinterleaver, RS(204,188), energy-dispersal reversal, TS-validatie en status JSON. |
| `tools/rbdvbt_status_watch.c` | Leest status JSON periodiek en toont lock, SNR, SSI/SQI, packet counters en service-info. |
| `tools/dvbt_fec_snr_plot.c` | Monte Carlo model voor QPSK AWGN, puncturing en soft Viterbi. |

## Decodeketen

De huidige praktische keten is:

```text
stdin IQ
  -> input parser en IQ-statistiek
  -> DC removal
  -> optionele resampling naar DVB-T grid
  -> optionele guard auto-detectie via CP-correlatie
  -> cyclic-prefix timing
  -> veilige FFT-start binnen CP plateau
  -> 2048 FFT
  -> DVB-T 2K logical carrier mapping
  -> pilot scan: bin shift, conjugate, symbol phase mod 4
  -> pilot-aided fasecorrectie en kanaalnormalisatie
  -> equalized QPSK punten
  -> QPSK soft demap
  -> DVB-T bit deinterleaving
  -> optionele FEC auto-detectie via Viterbi + outer TS score
  -> depuncturing voor FEC 1/2, 2/3, 3/4, 5/6, 7/8
  -> soft Viterbi
  -> outer byte deinterleaver
  -> Reed-Solomon RS(204,188)
  -> energy-dispersal reversal
  -> MPEG-TS packets
  -> TS-validatie en JSON-status
```

De naam `--probe-constellation` is historisch gegroeid. In de huidige code is
dit niet meer alleen een probe; het is de route waarin de volledige werkende
demodulatieketen zit.

## Voorgenomen Async Pipeline

Voor een volgende refactor splitsen we de receiver in vier stateful
verwerkingsblokken met FIFO's op semantische datagrenzen. Dit volgt dezelfde
hoofdles als LeanDVB: buffers horen op plekken waar de datavorm verandert, niet
midden in een feedback loop.

```text
SDR / IQ input
  -> optionele FIFO0: IQ ringbuffer
  -> Block 1: IQ -> OFDM/QAM demap
  -> FIFO1: soft-bit/cell batches
  -> Block 2: inner deinterleaver + depuncture + Viterbi
  -> FIFO2: Viterbi decoded byte stream
  -> Block 3: outer deinterleaver + Reed-Solomon + derandomizer
  -> FIFO3: complete 188-byte MPEG-TS packets
  -> Block 4: TS writer / streamer / analyzer
  -> FIFO4: optionele output/status queue
```

De kernrelatie tussen blokken is:

```text
Block 1 draait continu
Block 2 consumeert FIFO1
Block 3 consumeert FIFO2
Block 4 consumeert FIFO3
```

### Block 1: IQ naar demapper output

Block 1 is een stateful producer. Als er eenmaal lock is, verwerkt hij volgende
IQ chunks vanuit zijn bestaande tracking-state. Hij mag dus niet per chunk
opnieuw beginnen met volledige acquisitie.

Interne state van Block 1 blijft prive:

```text
sync_state: SEARCHING / LOCKED / HOLDOVER / LOST
sample_index
symbol_index
FFT window position
carrier frequency offset / PLL state
sample timing offset
guard interval alignment
channel estimate / equalizer taps
TPS/config state
frame counters
confidence metrics
```

De deterministische vorm is:

```text
Block1.process(iq_chunk, state_in) -> fifo1_items + state_out
```

Zolang de IQ-stream continu is:

```text
LOCKED state + volgende IQ chunk -> volgende OFDM symbols -> demap -> FIFO1
```

Een klein kwaliteitsverlies kan via `HOLDOVER` lopen zonder directe pipeline
flush. Een echte IQ-discontinuiteit of langdurig lockverlies gaat naar `LOST`,
verhoogt `generation_id` en triggert downstream reset.

FIFO1 bevat geen anonieme losse bits maar records:

```text
Fifo1Item {
  generation_id
  tps_config
  first_sample_index
  first_symbol_index
  symbol_count
  soft_bits
  quality_flags
}
```

### Block 2: inner FEC

Block 2 consumeert FIFO1 en doet:

```text
inner bit/symbol deinterleaver
  -> depuncturing
  -> soft Viterbi
  -> FIFO2
```

Block 2 houdt zijn eigen state lokaal:

```text
inner_deinterleaver buffers
puncturing phase
Viterbi trellis/path state
bit/byte alignment
```

FIFO2 bevat byte-georienteerde data:

```text
Fifo2Item {
  generation_id
  byte_offset
  bytes
  viterbi_metric
  ber_estimate
  flags
}
```

Bij een nieuwe `generation_id`, `DISCONTINUITY` of incompatible config reset
Block 2 zijn interne buffers voordat nieuwe data wordt geaccepteerd.

### Block 3: outer FEC en derandomizer

Block 3 consumeert FIFO2 en doet:

```text
MPEG/RS byte sync indien nodig
  -> outer byte deinterleaver
  -> Reed-Solomon RS(204,188)
  -> energy derandomizer
  -> FIFO3
```

De volgorde is bewust:

```text
outer deinterleaver -> Reed-Solomon -> derandomizer
```

Dit is ook de praktische volgorde zoals LeanDVB die gebruikt voor zijn
DVB-S-keten.

Block 3 houdt lokaal:

```text
TS/RS alignment
outer_deinterleaver memory
RS decoder state
derandomizer PRBS phase
packet counter
```

FIFO3 is de schoonste grens in de keten:

```text
TsPacket {
  generation_id
  packet_index
  bytes[188]
  rs_corrected_errors
  flags
}
```

### Block 4: TS sink

Block 4 doet geen DVB-FEC meer. Het consumeert complete TS packets uit FIFO3 en
handelt output en analyse af:

```text
continuity checks
file writer
UDP streamer
TS analyzer
status/statistics
```

FIFO4 is optioneel. Gebruik die alleen als de sink zelf ook asynchroon moet zijn,
bijvoorbeeld voor een writer, GUI of netwerkstream die los van TS-validatie mag
lopen.

### Informatiebus

Naast payload-FIFO's komt er een kleine informatiebus voor expliciete events en
status. De bus deelt geen interne algoritme-state, maar alleen informatie die
andere blokken nodig hebben om correct te resetten, monitoren of rapporteren.

Minimale events:

```text
CONFIG_CHANGED
SYNC_SEARCHING
SYNC_LOCKED
SYNC_HOLDOVER
SYNC_LOST
GENERATION_CHANGED
PIPELINE_FLUSH
FIFO_LEVEL
FIFO_OVERRUN
FIFO_UNDERRUN
DROPOUT
ERROR_RATE_UPDATE
```

Niet via de bus delen:

```text
PLL internals
equalizer taps
Viterbi survivor paths
outer deinterleaver memory
Reed-Solomon syndrome internals
derandomizer shift register
```

### Backpressure en gaten

Alle FIFO's zijn bounded. Onbeperkt bufferen maskeert alleen dat realtime
verwerking niet wordt bijgehouden.

Voor offline/file input:

```text
FIFO vol  -> producer wacht
FIFO leeg -> consumer wacht
```

Voor live SDR:

```text
FIFO0/FIFO1 overrun
  -> IQ_OVERRUN of FIFO_OVERRUN event
  -> DISCONTINUITY markeren
  -> generation_id++
  -> downstream pipeline flush
  -> Block 1 naar HOLDOVER of LOST
```

Een gat in FIFO1 mag nooit stilzwijgend door inner/outer FEC lopen. Downstream
consumeert alleen records waarvoor geldt:

```text
zelfde generation_id
symbol_index/byte_offset sluit aan
config is compatible
geen DISCONTINUITY flag
```

Als Block 1 te weinig levert, blijven Block 2-4 idle en publiceert de bus
underrun/starving status. Dat is normaal bij acquisitie, zwak signaal of
offline input die langzamer binnenkomt.

## Automatische Parameterdetectie

`--gi auto` gebruikt een praktische CP-correlatiescan. De receiver vergelijkt
`1/32`, `1/16` en `1/8` op de actuele sample-grid, dus na eventuele resampling.
De selectie begint bij de kortste guard en kiest alleen een langere guard als de
CP-score duidelijk beter is. Dit voorkomt dat `1/8` onterecht wint alleen omdat
er meer CP-samples in de correlatie zitten.

`--fec auto` probeert de ondersteunde puncturing rates en decodeert per kandidaat
de Viterbi-output. Wanneer TS-output actief is, wordt de kandidaat gekozen op de
outer receiver score:

- Reed-Solomon resultaat
- TS sync
- PAT/PMT aanwezigheid
- SDT/service-info
- continuity counters

Dit is bewust een praktische detectie. De DVB-T standaard draagt GI/FEC ook in
TPS-carriers, maar TPS-decode is nog niet de gebruikte detectieroute.

## Transport Stream

De outer laag zoekt eerst de juiste fasen:

- outer byte-deinterleaver fase
- RS blokfase
- MPEG-TS 8-packet energy-dispersal fase

Daarna schrijft de receiver 188-byte TS packets. De TS validator telt:

- sync byte fouten
- transport error indicator
- continuity counter fouten
- PAT packets
- PMT packets
- SDT packets
- service id / program id
- video/audio PID
- service name en provider

Belangrijk meetresultaat:

```text
TS recovery is functioneel; playback wordt nu beperkt door resterende payload errors.
```

Dat bleek uit `ffprobe`: de TS-structuur, PAT/PMT en H.264 stream werden herkend,
maar H.264 gaf nog payload-corruptie zoals ontbrekende PPS en slice decode errors.
Later leverde `ffplay` al een compleet beeld. De resterende kwaliteit wordt dus
vooral bepaald door packet errors in de payload, niet meer door ontbrekende
transportstructuur.

## Status JSON

De receiver kan periodiek status naar JSON schrijven met:

```text
--status-json FILE
--status-period-packets N
```

De status start bij nul/leeg zolang er geen bruikbare lock of TS-validatie is.
Bij lock worden onder andere gevuld:

- `symbol_rate`
- `modulation`
- `fft`
- `constellation`
- `fec`
- `guard`
- `locked`
- `lock_quality`
- `pilot_lock`
- `ssi`
- `sqi`
- `snr`
- `packets`
- `rs_corrected`
- `rs_uncorrectable`
- `pat_packets`
- `pmt_packets`
- `sdt_packets`
- `service_id`
- `program_id`
- `service_name`
- `service_provider`

`SSI` en `SQI` moeten bij geen lock `0` zijn. De JSON wordt atomisch geschreven
via een tijdelijk bestand en daarna `rename`, zodat de statuswatcher geen half
geschreven JSON hoeft te lezen.

## Belangrijkste Doorbraken

### 1. Het signaal is geen kleine custom OFDM

De eerste verkeerde aanname was dat Portsdown een kleine reduced-bandwidth OFDM
met een paar honderd carriers maakt. Dat leidde tot FFT512-achtige probes en
chaotische constellaties.

De Portsdown-code liet zien dat het signaal logisch een standaard DVB-T 2K grid
gebruikt:

- 1705 active carriers
- 1512 data cells per OFDM symbol
- continual pilots
- scattered pilots
- TPS carriers
- DVB-T PRBS pilotpolariteit
- standaard symbol interleaver

Reduced bandwidth ontstaat door de sample/timing configuratie rond die logische
2K structuur, niet doordat de logische carrierstructuur verdwijnt.

Praktische regel:

```text
Altijd eerst het DVB-T 2K logical carrier model gebruiken.
Niet proberen een klein los OFDM-model uit de spectrumplaat te raden.
```

### 2. Een chaotische wolk betekent niet automatisch dat het signaal fout is

De eerste QPSK-plots zagen eruit als een chaotische wolk. Dat kwam doordat data,
pilots, TPS, timingfouten, fasehelling en verkeerde carrierrollen door elkaar
werden geplot.

Wat hielp:

- CP timing gebruiken voor symboolstart.
- FFT-start niet op de CP-correlatiepiek zetten, maar veilig binnen het CP plateau.
- DVB-T carrierrollen scheiden.
- Pilot-lock gebruiken om bin shift, conjugate en symbol phase mod 4 te bepalen.
- Per symbool fase en slope corrigeren.
- Daarna pas data carriers als QPSK plotten.

Praktische regel:

```text
Een constellatieplot is pas bewijs na timing, carrierrol en pilotcorrectie.
```

### 3. CP timing: plateau belangrijker dan piek

De cyclic-prefix correlatie geeft een timingplateau. De hoogste piek is niet
altijd de beste FFT-start. Een startpunt veilig binnen het plateau gaf betere
equalized constellaties en stabielere demapping.

In de code wordt daarom een plateau rond de CP-correlatie gezocht en een
conservatief punt binnen dat plateau gekozen.

Praktische regel:

```text
Kies de FFT-start binnen het CP plateau, niet blind op de hoogste correlatiepiek.
```

### 4. Pilot-lock was de sleutel tot bruikbare QPSK

De grote verbetering kwam door de DVB-T pilots actief te gebruiken:

- continual pilots
- scattered pilots met `3 * (symbol mod 4) + 12p`
- PRBS pilot sign
- scan over bin shift
- scan over conjugate/no-conjugate
- scan over symbol phase mod 4

Hiermee werd de cirkel/wolk een bruikbare QPSK constellatie en werd demapping
realistisch.

Praktische regel:

```text
Bij nieuwe captures eerst pilot_lock maximaliseren; daarna pas demappen.
```

### 5. Zelfgemaakte Portsdown-IQ is nuttig als referentie

Het was nuttig om de Portsdown DVB-T zendercode te gebruiken om eigen IQ te
maken. Daarmee konden we transmitterlogica, carrierrollen en FEC-volgorde
controleren zonder onzekerheid van de etheropname.

Praktische regel:

```text
Gebruik synthetische Portsdown-IQ als regressietest voordat een nieuwe off-air
capture wordt verdacht.
```

### 6. Eerst TS-structuur bewijzen, daarna beeldkwaliteit

Toen `ffprobe` PAT/PMT, H.264 en service-info kon vinden, was duidelijk dat de
receiver de TS-structuur functioneel terugwon. De resterende fouten zaten in
payload-integriteit.

Praktische regel:

```text
Debug volgorde:
1. TS sync 0x47
2. PAT/PMT/SDT
3. continuity counters
4. RS corrected/uncorrectable
5. H.264/AAC payload decode
```

## Nieuwe IQ-Opname Snel Decoderen

Gebruik deze volgorde bij een nieuwe opname:

1. Start met bekende parameters: sample rate, `--sr`, `--gi`, `--fec`.
2. Gebruik `--probe-constellation --resample-to-dvbt-rate --dvbt-ir 1`.
3. Schrijf eerst diagnostiek:

```text
--cp-timing-out
--cp-timing-svg
--constellation-out
--constellation-svg
--equalized-out
--equalized-svg
--demap-out
--viterbi-out
```

4. Controleer CP timing:

- Is er een duidelijk plateau?
- Is de veilige start binnen het plateau?
- Is de CFO uit CP-correlatie plausibel?

5. Controleer pilot-lock:

- Is er een duidelijke beste bin shift?
- Is conjugate correct gekozen?
- Is symbol phase mod 4 stabiel?

6. Controleer equalized QPSK:

- Vier clusters zichtbaar?
- Geen ringvormige wolk?
- Geen sterke fasehelling over carriers?

7. Pas daarna `--ts-out` of `--stdout-ts` gebruiken.
8. Valideer met `ffprobe` of `ffplay`.
9. Kijk bij fouten eerst naar RS/TS counters, niet alleen naar beeld.

## Parameterinzichten

### Guard interval

Guard interval helpt tegen echo-delay/multipath, maar niet tegen fading door
interferentie tussen tropo/direct en aircraft-scatter paden.

Praktische regel:

```text
Gebruik de kortste guard die de echo-delay opvangt.
Bij moment/fading problemen liever lagere symbolrate of sterkere FEC kiezen.
```

`GI 1/32` is standaard de beste keuze wanneer echo-delay niet het hoofdprobleem
is. `GI 1/8` kost overhead en gaf in de performancegrafieken alleen zin als die
extra guard werkelijk multipath buiten `1/32` opvangt.

### Symbolrate

Bij gelijke totale zendpower heeft een lagere symbolrate meer energie per
symbool. Voor aircraft scatter is signaalsterkte vaak niet het enige probleem;
het bruikbare moment en fading zijn meestal bepalender.

Praktische regel:

```text
333k voor throughput, 250k als praktische middenweg, 150k voor korte/fadende momenten.
```

Let op: de performance-simulatietool modelleert 150k, 250k en 333k. De huidige
receiver-CLI accepteert in de code nog de presets 125k, 250k, 333k en 500k.

### FEC

FEC 1/2 is het meest robuust, FEC 2/3 is een goede standaard, FEC 3/4 vraagt
meer marge. 5/6 en 7/8 zijn vooral nuttig als de link al zeer stabiel is.

## Performancegrafieken

De grafieken in `plots/` zijn Monte Carlo simulaties van:

```text
random 188-byte packets
  -> convolutional encoder
  -> puncturing
  -> QPSK AWGN kanaal
  -> soft Viterbi
  -> packet bit-perfect check
```

Belangrijke correctie in de meetketen:

- totale signaalpower constant tussen symbolrates
- noise spectral density constant
- C/N in vaste meetbandbreedte
- omrekening naar Es/N0/Eb/N0 per symbolrate

Daardoor wordt zichtbaar dat lagere symbolrates beter presteren bij gelijke
totale zendpower. De README gebruikt:

- `plots/fec_packet_error_vs_fixed_cn_by_sr_150k.svg`
- `plots/fec_packet_error_vs_fixed_cn_by_sr_150k_gi18.svg`

## Bekende Technische Schuld

- De volledige decode loopt nog via de historisch genoemde
  `--probe-constellation` route. Dat zou later een normale decode-mode moeten
  worden.
- TPS wordt nog niet gebruikt voor autodetectie van FEC/GI/mode.
- De receiver-CLI mist nog een `150k` preset terwijl de performance-analyse die
  mode wel gebruikt.
- `src/probe_constellation.c` bevat veel verantwoordelijkheden in een groot
  bestand. Functioneel was dit nuttig tijdens de doorbraakfase, maar later kan
  het gesplitst worden in acquisition, OFDM, equalizer, demap en inner-FEC.
- De status/SNR-waarden zijn praktische receiver-metrics, geen gekalibreerde
  meetontvangerwaarden.

## Bestanden die zijn samengevoegd in dit document

De oude documenten `CONTEXT.md`, `DEVELOPMENT.md`,
`DVB-T-generation.md` en `dvbt_gnuradio_receiver_first_stage_calculations.md`
waren nuttig tijdens de analysefase, maar hun blijvende inhoud is hier
samengevat:

- doel en Unix-pipeline karakter
- stdout/stderr discipline
- staged DSP-aanpak
- Portsdown transmittervolgorde
- resampling/timing/pilot lessen
- doorbraakmomenten richting werkende TS recovery
