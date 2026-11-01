# Protocole de NovaAudioEngine (version 1)

NovaAudioEngine est un **serveur audio** autonome. Un client (n'importe quel programme) le lance,
lui envoie des commandes et échange l'audio avec lui. Le même protocole est parlé par les trois
éditions du moteur (`edition` dans `ready` / `ping`) : **asio** et **libre** (`NovaAudioEngine.exe`,
logiciel libre GPLv3, sans Nova Tune) et **pro** (`NovaProAudio.exe`, « Mode audio pro Nova »,
logiciel propriétaire de Make Music, sans ASIO, avec Nova Tune natif ; ajouts au § 6). Ce document
est le **seul contrat** entre le moteur et ses clients : le moteur ne connaît ni Nova Studio, ni ses
projets, pistes, effets ou formats internes, seulement des notions génériques de serveur audio
(périphérique, flux, canaux, tampons, latences, fichiers de prise, chaîne de retour casque).

Dans Nova Studio, le client est le pont Python (`bridge-python/native_audio.py`, propriétaire) ;
l'interface de Nova ne parle jamais au moteur directement.

## 1. Transport

| Canal | Contenu |
|---|---|
| ligne de commande | aucun argument (un seul argument interne : `--ipc-echo <nom>`, processus écho de `bench_ipc`) |
| entrée standard | une commande JSON par ligne (UTF-8, fin de ligne `\n`) |
| sortie standard | une réponse ou un événement JSON par ligne |
| mémoire partagée Windows | audio et compteurs du flux (section 4), nommée par la réponse à `start` |
| fichiers | prises WAV écrites par le moteur aux chemins donnés par le client |

Le moteur ne lit aucun autre fichier du client et n'ouvre aucune connexion réseau. Si l'entrée
standard se ferme (client arrêté ou planté), il arrête le flux, ferme proprement les prises en
cours et quitte (au plus 6 s).

### Messages

- Commande : `{"id": <nombre ou texte>, "cmd": "<nom>", ...paramètres}`
- Réponse : `{"id": ..., "ok": true, ...résultat}` ou `{"id": ..., "ok": false, "error": "<texte>"}`
  (une réponse par commande ; `rec_stop`, `measure_latency` et `bench_ipc` répondent plus tard,
  les autres réponses peuvent donc arriver dans le désordre : le client les associe par `id`).
- Événement : `{"event": "<nom>", ...}` (sans `id`).

Événements : `ready` (au lancement : `version`, `protocol`, `features`, `edition`, `asio` = ASIO compilé (absent dans l'édition pro), `pid`),
`device` (voir § 3), `log` (`message`), `bye` (fin).

## 2. Commandes

| Commande | Paramètres | Résultat |
|---|---|---|
| `ping` | — | `version`, `protocol` (= 1), `edition` (`pro`, `asio`, `libre`), `features` (ajouts compatibles : `monitor_chain` ; édition pro seulement : `novatune`, `monitor_params`, `pro_audio`, `pro_calibrate`, `device_recovery`, `aggregate`, `measured_latency`), `asio` (absent dans l'édition pro) |
| `devices` | `wasapi?` (booléen) | `asio: [{name, registered}]`, `asio_available`, `wasapi?: […]`, `simulated: ["null","file"]` |
| `start` | `device`, `project_rate`, `device_rate?`, `buffer`, `in_channels?`, `out_channels?`, `capture_seconds?`, `play_min_blocks?`, `measured_roundtrip?`, `stress?` | voir ci-dessous et § 6.3 |
| `pro_calibrate` | édition pro : § 6.2 | assistant « meilleur réglage » |
| `stop` | — | `{}` |
| `stats` | — | compteurs du flux (`running`, `device`, `device_type`, `project_rate`, `device_rate`, `buffer`, `in_channels`, `out_channels`, `input_latency`, `output_latency`, `monitor_latency`, `callbacks`, `late_callbacks`, `callback_avg_us`, `callback_max_us`, `device_xruns`, `device_lost_frames`, `capture_lost_frames`, `play_underruns`, `play_dropped_frames`, `play_target_blocks`, `resampling`, `inserts`…) |
| `routes` | `enabled`, `routes: [{in, out, gain}]` | `routes` (nombre) : retour direct entrée → sortie, sans effet |
| `monitor_chain` | `strips: [...]` ou `clear: true` | rapport de chargement (§ 2.3) |
| `monitor_params` | `strip`, `insert?` (id ou n°, défaut `"novatune"`), `params: {…}` | `strip`, `id`, `latency`, `chain_latency`, `params` : réglages d'un insert Nova Tune, sans reconstruire la chaîne (§ 2.3) |
| `measure_latency` | `outs: [n…]`, `seconds?`, `amp?` | `delays: [par entrée, en images, ou null]`, `sample_rate` |
| `rec_start` | `take_id`, `files: [{path, track_id?, inputs, mode?}]`, `preroll?`, `start_frame?` | `take_id`, `start_frame`, `now_frame`, `sample_rate`, `device_rate` |
| `rec_stop` | `take_id`, `end_frame?` | `take_id`, `files: [{path, track_id, frames, channels}]`, `losses: [{frame, lost_frames, kind}]`, `start_frame`, `end_frame`, `sample_rate` |
| `rec_status` | — | `takes: [...]` (prises en cours) |
| `control_panel` | — | `{}` : ouvre le panneau du pilote ASIO en cours |
| `bench_ipc` | `seconds?`, `frames?`, `rate?` | statistiques d'aller-retour entre processus (µs), outil de mesure |
| `quit` | — | `{}`, puis le moteur s'arrête |

### 2.1 Périphériques (`device`)

Chaîne `type:nom?option=valeur&…` :

- `asio:<nom du pilote>` (ou le nom seul) : pilote ASIO inscrit dans Windows, chargé en lecture seule
  depuis le registre ;
- `asio-dll:<chemin>?clsid={…}` : pilote ASIO chargé directement depuis une DLL (tests) ;
- `wasapi` ou `wasapi:<id du point de terminaison>` (`?exclusive=1`, `?in=<id d'entrée>`) : carte Windows standard ;
- `null` (`?input=silence|…`, `?loopback=1`) et `file?in=<wav>&out=<wav>&loop=1` : périphériques simulés (tests, aucun son réel) ;
- `pro[:<id de sortie>]` (édition pro seulement, § 6.1) : mode audio pro Nova. Dans l'édition pro, `asio:` et
  `asio-dll:` n'existent pas (« périphérique inconnu »).

### 2.2 `start`

Ouvre la carte avec le tampon demandé, à `device_rate` (0 = fréquence courante de la carte).
Si la carte ne tourne pas à `project_rate`, le moteur rééchantillonne dans les deux sens : le client
ne voit que `project_rate`. Réponse :

- `device` : `{type, name, format, sample_rate, rates, buffer, min_buffer, max_buffer,
  preferred_buffer, granularity, in_channels, out_channels, in_names, out_names, input_latency,
  output_latency, output_ready}` (latences en images de la carte, annoncées par le pilote) ;
- `shm` : nom de la mémoire partagée ; `layout` : décalages de tous ses champs (§ 4) ;
- `project_rate`, `device_rate`, `buffer`, `in_channels`, `out_channels`, `resampling`,
  `resampler?`, `input_latency`, `output_latency`, `roundtrip_ms`.

### 2.3 `monitor_chain` (retour casque avec effets)

```json
{ "strips": [ { "id": "a", "inputs": [0] , "gain_db": 0,
                "sends": [ { "outs": [2, 3], "gain": 1.0 } ],
                "inserts": [ { "type": "vst3", "path": "C:/…/Effet.vst3", "class": "…", "cid": "…",
                               "state": { "component": "<base64>", "controller": "<base64>" } },
                             { "type": "gain", "db": -6 } ] } ] }
```

Une tranche par entrée (mono ou stéréo) : gain, inserts traités dans le rappel de la carte, envois
vers des sorties. `state` est l'état binaire standard VST3 du module (opaque pour le moteur). Un
module qui plante passe en direct (signal sec), le moteur continue. Réponse : latence de la chaîne
et état de chaque insert.

**Insert `"novatune"`** (correcteur de hauteur intégré au moteur, `Source/NovaTune.*`) : aucun
fichier chargé, calcul dans le rappel de la carte, somme mono de la tranche corrigée puis rendue
sur tous ses canaux.

```json
{ "type": "novatune", "id": "novatune",
  "params": { "speed": 0.1, "mix": 1, "humanize": 0.2, "rootKey": 0, "scale": "CHROMATIC",
              "isEnabled": true, "lowLatency": true, "flex": 0, "vibrato": 0, "formant": 0,
              "transpose": 0, "voiceType": "auto", "targetNotes": [], "chord": 0 } }
```

| Réglage | Valeurs | Rôle |
|---|---|---|
| `speed` | 0–1 | vitesse de correction : 0 = instantanée (robot), 1 ≈ 300 ms |
| `mix` | 0–1 | dosage de la correction (0 = voix non corrigée) |
| `humanize` | 0–1 | ralentit la correction sur les notes tenues |
| `rootKey` | 0–11 | tonique (Do = 0) |
| `scale` | `CHROMATIC`, `MAJOR`, `MINOR`, `MINOR_HARMONIC`, `PENTATONIC` (ou 0–4) | gamme |
| `isEnabled` | booléen | faux = signal intact (bypass) |
| `lowLatency` | booléen, **vrai par défaut** | mode direct (0,5 ms + au plus une période) ; faux = mode qualité PSOLA (~27 ms) |
| `flex` | 0–1 | ne corrige que près de la note |
| `vibrato` | 0–2 | part du vibrato chanté gardée |
| `formant` | −12–12 demi-tons | timbre (mode qualité seulement) |
| `transpose` | −12–12 demi-tons | transposition après correction |
| `voiceType` | `auto`, `grave`, `aigue` (ou 0–2) | plage de détection (75–1000, 75–400, 140–1000 Hz) |
| `targetNotes` | classes 0–11 | notes cibles (vide = la gamme) |
| `chord` | tonique × 4096 + masque 12 bits, 0 = aucun | accord en cours (notes visées) |

Absent = valeur par défaut ; hors bornes = ramené aux bornes. Latence annoncée : 0,5 ms + 4 images
+ une demi-période d'une voix à 150 Hz en mode direct (188 images à 48 kHz ; mesurée 28 images sur
une impulsion, 0,73 ms sur une voix), `2 × ⌈fréquence / 75⌉ + 34` en mode qualité (1 314 images à
48 kHz). `monitor_params` change un ou plusieurs réglages au bloc suivant, sans coupure ; ils sont
gardés si le flux est rouvert. `stats` donne pour chaque insert Nova Tune `detected_hz`,
`target_hz`, `correction_cents` et `params`.

### 2.4 Prises

`rec_start` ouvre un fichier WAV 32 bits flottants par entrée de `files` (`mode` : `mono`, `stereo`
ou `sum` ; `inputs` : n° d'entrées de la carte). Les images sont numérotées depuis le démarrage du
flux, à `project_rate` (même numérotation que l'anneau « aperçu ») ; `start_frame` permet de
reprendre en arrière (au plus la durée de `capture_seconds`), `preroll` sinon (1 s par défaut).
`track_id` est une étiquette libre recopiée telle quelle dans la réponse. Les pertes éventuelles
(images que la carte a perdues) sont remplacées par du silence au bon endroit et listées dans
`losses`.

## 3. Événement `device`

`{"event": "device", ended?, reset?, resync?, latency_changed?, overload?, error?, restarted?, restart_error?}`.
Sur `reset` ou `latency_changed` (le pilote a changé de tampon ou de fréquence), le moteur rouvre
lui-même le flux avec les mêmes réglages : `restarted` contient alors la nouvelle réponse de
`start` (nouvelle mémoire partagée) ; les prises en cours ont été fermées proprement.

## 4. Mémoire partagée

Créée par le moteur (« `Local\NovaAudio-<pid>-<n>` »), little-endian, x64. Le client doit lire les
décalages dans `layout` (réponse à `start`) et refuser la zone si `magic` (`0x3141564E`) ou `version`
(`1`) ne correspondent pas. Champs (`SharedEngine.h`) :

- flux : `running`, `projectRate`, `deviceRate`, `blockFrames`, `inChannels`, `outChannels`,
  `inputLatency`, `outputLatency`, `playLookahead` ;
- horloge : `clockSeq` (pair = stable, comme un seqlock), `clockFrame` (n° d'image) ↔ `clockTime`
  (secondes du compteur haute résolution de Windows, QueryPerformanceCounter) ;
- compteurs 64 bits : `callbacks`, `deviceFrames`, `lateCallbacks`, `deviceXruns`,
  `deviceLostFrames`, `captureLostFrames`, `playUnderruns`, `playDroppedFrames`, `playFrames`,
  `cbLastNs`, `cbMaxNs`, `cbTotalNs`, `playTargetBlocks`, `playMinBlocks`, `monitorLatency` ;
- crêtes : `inPeak[32]`, `outPeak[32]` (flottants ; le client peut les remettre à zéro) ;
- deux anneaux `RingState` de 64 octets : `write`, `claim`, `read` (64 bits, positions ABSOLUES en
  images), `capacity` (puissance de 2), `channels`, `dataOffset` (données entrelacées, flottants
  32 bits) :
  - **`preview`** (moteur → client), mode « écrasement » : les entrées de la carte à
    `project_rate`. Le moteur n'attend jamais : il publie `claim` avant d'écrire puis `write` ; le
    client copie, relit `claim` et jette ce qui a pu être réécrit pendant sa copie.
  - **`play`** (client → moteur), mode « borné » : l'audio à jouer, une colonne par sortie. Le
    client écrit seulement dans la place libre (`write − read < capacity`) et publie `write` ; le
    moteur avance `read`. Marge de départ `play_min_blocks`, adaptée par le moteur
    (`playTargetBlocks`) ; file vide en plein jeu = `playUnderruns`.

## 5. Évolution

Toute modification incompatible augmente `protocol` (réponse à `ping` et événement `ready`) et,
pour la mémoire partagée, `version`. Un client doit refuser une version qu'il ne connaît pas et
revenir à son propre chemin audio (c'est ce que fait Nova Studio).

## 6. Édition pro : « Mode audio pro Nova » (ajouts compatibles, 30/10/2026)

Un client qui ne connaît pas ces ajouts n'a rien à changer : ils n'apparaissent que si `features`
les annonce, et les commandes / champs existants gardent leur sens (`monitor_chain`, `novatune`,
`monitor_params`, `features` : mêmes noms, mêmes réponses dans les deux éditions qui les ont).

### 6.1 Périphérique `pro`

`pro[:<id de sortie>]?in=<id d'entrée>|none&exclusive=auto|1|0&drift=auto|on|off&inmap=0,1&outmap=0,1`

- sortie : point de terminaison Windows donné, sinon celui par défaut (le flux le **suit** : changé dans
  Windows, il est rouvert sur le nouveau) ; entrée : `in=`, sinon l'entrée par défaut, `none` = aucune ;
- `exclusive=auto` (défaut) : WASAPI **exclusif** piloté par événements, période demandée ramenée à la
  plus petite que le périphérique accepte (`min_buffer`, GetDevicePeriod ; alignement imposé par le pilote
  respecté) ; s'il est impossible (carte prise par une autre application, exclusif interdit, format
  refusé) : **partagé faible latence** (IAudioClient3, période multiple de la période fondamentale,
  `granularity`), sinon partagé classique. `1` : exclusif ou échec ; `0` : jamais d'exclusif ;
- deux fils MMCSS « Pro Audio » à priorité critique (rendu, capture), sans allocation ni verrou ;
- formats essayés tels quels : float 32, entier 24 utiles sur 32, 24 tassés, 32, 16 bits ; fréquence
  demandée, sinon 48 / 44,1 / 96 / 88,2 kHz (le moteur rééchantillonne alors vers le projet) ;
- `inmap` / `outmap` : canaux de la carte pris, dans l'ordre ; `device.in_names` / `out_names` :
  « <carte> · Gauche », « <carte> · Entrée 3 »… ;
- entrée et sortie de deux boîtiers différents (ContainerId) : **agrégation** avec correction de dérive
  d'horloge (`drift=auto`) ; `on` la force, `off` l'interdit ; même boîtier : file directe sans latence
  ajoutée (et si une dérive apparaît quand même, la correction s'enclenche seule) ;
- `device.format` : `exclusif int24`, `partagé faible latence float32`… ; `stats.device_stats` : `mode_out`,
  `mode_in`, `format_out`, `format_in`, `period_frames`, `capture_period_frames`, `min_period_frames`,
  `default_period_frames`, `same_clock`, `aggregate`, `drift_ppm`, `fifo_level`, `fifo_target`,
  `late_wakes` (périodes perdues : réveil en retard), `render_underruns`, `capture_discontinuities`,
  `fifo_starved`, `fifo_overflow`, `max_callback_us`, `max_wake_jitter_us`, `lost`, `exclusive_refused?` ;
- `devices` avec `wasapi: true` : `pro: [{id, name, flow, default, container, channels, mix_rate,
  channel_names, min_period_ms, default_period_ms}]` (inventaire, aucun flux ouvert), `pro_available` ;
- pertes de la carte dans `rec_stop.losses` : `kind` = `"carte"` (au lieu de `"asio"`).

### 6.2 `pro_calibrate` (assistant)

`{cmd: "pro_calibrate", device?, project_rate?, buffers?: [..], seconds?: 4, stress?: 0.3, margin_steps?: 1,
loop?: {out, in}, apply?: true}` — réponse différée (quelques secondes par taille). Le flux en cours est
arrêté ; chaque taille (de la plus grande à la plus petite, défaut 1024 … 16 ; sous le minimum du
périphérique : sautée) tourne `seconds` s dans un moteur d'essai avec une charge simulée (`stress` = part
de la période) ; coupures = `device_xruns` + `late_callbacks` ; deux échecs de suite arrêtent la descente.
Avec `loop`, l'aller-retour est mesuré par impulsion (câble sortie `out` -> entrée `in`, ou entrée
« boucle » de la carte). Réponse : `trials: [{requested, buffer, ok, xruns, device_xruns, late_callbacks,
callback_avg_us, callback_max_us, cpu_percent, announced_roundtrip_ms, roundtrip_frames?, roundtrip_ms?,
device_stats}]`, `smallest_clean`, `chosen` (`margin_steps` crans au-dessus), `measured_roundtrip`
(images de la carte, ou `null`), `latency_source` (`measured` | `announced`), `device` ; si `apply`, le flux
est rouvert au réglage choisi avec la latence mesurée : `restarted` (réponse de `start`, nouvelle mémoire
partagée) ou `restart_error`. Refusé pendant une prise.

### 6.3 Latence mesurée et charge

`start` accepte `measured_roundtrip` (images de la carte, pour ce tampon) : `input_latency` +
`output_latency` publiés (réponse, `stats`, mémoire partagée) valent alors exactement cette mesure
(l'écart avec l'annonce du pilote va à l'entrée, qui décale les prises) ; `latency_source` = `measured`,
`announced_roundtrip_ms` garde l'annonce. `stress` (0–0,9) : charge de calcul simulée dans le rappel (bancs).

### 6.4 Reprise automatique (`device_recovery`)

Événements `device` en plus : `{lost: true, recovering: true}` (carte débranchée ou invalidée, plus aucun
événement : veille, pilote bloqué) — le moteur ferme proprement les prises en cours et réessaie seul
(toutes les 0,5 s, puis 2 s) ; `{recovered: true, attempts, restarted}` quand elle revient (nouvelle mémoire
partagée, mêmes réglages). `{default_changed: true, restarted}` : périphérique par défaut de Windows changé
(flux qui le suit). Un `restart_error` accompagné de `recovering: true` n'est pas définitif. `stats` :
`recovering`, `recoveries`.
