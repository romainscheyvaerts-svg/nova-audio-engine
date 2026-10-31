# Protocole de NovaAudioEngine (version 1)

NovaAudioEngine est un **serveur audio** autonome. Un client (n'importe quel programme) le lance,
lui envoie des commandes et échange l'audio avec lui. Ce document est le **seul contrat** entre le
moteur (logiciel libre, GPLv3) et ses clients : le moteur ne connaît ni Nova Studio, ni ses
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

Événements : `ready` (au lancement : `version`, `protocol`, `asio` = ASIO compilé, `pid`),
`device` (voir § 3), `log` (`message`), `bye` (fin).

## 2. Commandes

| Commande | Paramètres | Résultat |
|---|---|---|
| `ping` | — | `version`, `protocol` (= 1), `asio` |
| `devices` | `wasapi?` (booléen) | `asio: [{name, registered}]`, `asio_available`, `wasapi?: […]`, `simulated: ["null","file"]` |
| `start` | `device`, `project_rate`, `device_rate?`, `buffer`, `in_channels?`, `out_channels?`, `capture_seconds?`, `play_min_blocks?` | voir ci-dessous |
| `stop` | — | `{}` |
| `stats` | — | compteurs du flux (`running`, `device`, `device_type`, `project_rate`, `device_rate`, `buffer`, `in_channels`, `out_channels`, `input_latency`, `output_latency`, `monitor_latency`, `callbacks`, `late_callbacks`, `callback_avg_us`, `callback_max_us`, `device_xruns`, `device_lost_frames`, `capture_lost_frames`, `play_underruns`, `play_dropped_frames`, `play_target_blocks`, `resampling`, `inserts`…) |
| `routes` | `enabled`, `routes: [{in, out, gain}]` | `routes` (nombre) : retour direct entrée → sortie, sans effet |
| `monitor_chain` | `strips: [...]` ou `clear: true` | rapport de chargement (§ 2.3) |
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
- `null` (`?input=silence|…`, `?loopback=1`) et `file?in=<wav>&out=<wav>&loop=1` : périphériques simulés (tests, aucun son réel).

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
