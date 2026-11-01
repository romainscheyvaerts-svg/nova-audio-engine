# NovaAudioEngine

Moteur audio natif de Nova Studio, en **trois éditions** construites depuis les mêmes sources :

| Édition | Exécutable | Contenu | Licence |
|---|---|---|---|
| `asio` (`build.bat asio`) | `NovaAudioEngine.exe` | pilotes ASIO (SDK de Steinberg), WASAPI de base | **GNU GPL v3** (`LICENSE`, `LICENCES.md`) — **sans** Nova Tune ni mode pro |
| `libre` (`build.bat`) | `NovaAudioEngine.exe` | WASAPI de base, périphériques simulés | GNU GPL v3 |
| `pro` (`build.bat pro`) | `NovaProAudio.exe` | **Mode audio pro Nova** (`Source/pro/`) + **Nova Tune natif** ; AUCUN code du SDK ASIO | **propriétaire, Make Music** (`pro/LICENCE.md`) ; SDK VST3 sous MIT |

Make Music est seul auteur du code du moteur : le code commun est publié sous GPLv3 dans les
éditions asio / libre et sous la licence de Make Music dans l'édition pro. `Source/pro/*` et
`Source/NovaTune.*` ne sont compilés QUE dans l'édition pro (garde-fou dans `CMakeLists.txt`) et ne
doivent pas être publiés (liste complète : `pro/LICENCE.md` § 7).

Ce qui suit décrit l'édition GPLv3 ; le mode pro est décrit au § « Mode audio pro Nova » plus bas et
dans `PROTOCOLE.md` § 6. Programme Windows autonome, écrit en C++17 **sans
JUCE**, qui joue le rôle d'un **serveur audio** : il ouvre la carte son (ASIO, WASAPI, ou
périphériques simulés), fait circuler l'audio par une mémoire partagée, écrit les prises en WAV,
assure le retour casque (direct ou avec effets VST3) et rééchantillonne si la carte ne tourne pas à
la fréquence demandée.

Il ne dépend d'aucune structure interne de Nova Studio : un client quelconque le pilote par le
protocole décrit dans **`PROTOCOLE.md`**. Dans Nova Studio, le client est le pont Python
(`bridge-python/native_audio.py`, propriétaire), utilisé seulement quand l'utilisateur allume
« Moteur audio natif (faible latence) » dans les Réglages audio.

## Contenu

| Chemin | Rôle |
|---|---|
| `Source/Main.cpp` | processus, boucle de commandes JSON (stdin / stdout) |
| `Source/Engine.*` | flux, anneaux en mémoire partagée, prises, mesure de latence |
| `Source/SharedEngine.h`, `Source/Rings.h` | disposition de la mémoire partagée et anneaux sans verrou |
| `Source/Device*.{h,cpp}` | périphériques : ASIO, WASAPI, nul, fichier |
| `Source/MonitorChain.*` | retour casque avec inserts VST3 dans le rappel de la carte |
| `Source/NovaTune.*` | Nova Tune natif : correcteur de hauteur de Nova Studio, insert `novatune` de la chaîne du casque |
| `Source/Resampler.*`, `Source/WavFile.*` | rééchantillonneur polyphase, fichiers WAV |
| `tests/Tests.cpp` | tests unitaires (console, aucun son réel) |
| `tests/NovaTuneBench.cpp` | banc de Nova Tune natif (fichiers WAV -> fichiers WAV, à travers la vraie chaîne du casque) |
| `fake-asio/` | pilote ASIO **factice** pour les tests (jamais livré) |
| `../native-host/Source/VstInstance.*`, `Guard.*`, `SharedAudio.h`, `../native-common/*` | fichiers partagés avec les hôtes VST de Nova, compilés dans le moteur (GPLv3 dans ce cadre) |

## Construire l'exécutable (source complète correspondante)

Prérequis : Windows 10/11 x64, **Visual Studio 2022** (ou Build Tools) avec « Développement Desktop
en C++ », **CMake ≥ 3.22** et **Ninja** (fournis avec Visual Studio), Git.

1. **SDK VST3 de Steinberg (MIT)**, version exacte `v3.8.1_build_84` :

   ```bat
   git clone --depth 1 --branch v3.8.1_build_84 https://github.com/steinbergmedia/vst3sdk.git "%NOVA_LIBS%\vst3sdk"
   git -C "%NOVA_LIBS%\vst3sdk" submodule update --init --depth 1 -- base cmake pluginterfaces public.sdk
   ```

   (`NOVA_LIBS` : un dossier de votre choix ; seuls les fichiers listés dans
   `../native-common/vst3_hosting.cmake` sont compilés, sans modification.)

2. **SDK ASIO de Steinberg 2.3.4** (double licence propriétaire / **GPLv3** ; utilisé ici sous
   GPLv3) : télécharger l'archive officielle depuis https://www.steinberg.net/asiosdk
   (lien direct au 22/10/2026 : `https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip`,
   SHA-256 `d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca`) et la décompresser ;
   `NOVA_ASIO_SDK` désigne le dossier qui contient `common\asio.h`. Seuls trois en-têtes sont
   compilés, tels quels : `common/asio.h`, `common/iasiodrv.h`, `common/asiosys.h`
   (`asio.cpp`, `asiodrivers.cpp` et `asiolist.cpp` ne sont pas utilisés). Le SDK n'est pas copié
   dans ce dépôt ; si le lien officiel disparaît, Make Music en fournit la copie exacte sur demande
   (offre écrite de `LICENCES.md`).

3. Construire :

   ```bat
   set NOVA_LIBS=C:\chemin\vers\libs
   set NOVA_ASIO_SDK=C:\chemin\vers\ASIOSDK
   native-audio\build.bat asio
   ```

   Résultat : `native-audio\build\NovaAudioEngine_artefacts\Release\NovaAudioEngine.exe`, plus
   `build\tests\Release\NovaAudioTests.exe` (tests, à lancer tels quels : « N réussis, 0 échecs »)
   et le pilote factice `NovaFakeAsio.dll` (tests seulement), ainsi que `build\tests\Release\NovaTuneBench.exe`
   (banc de Nova Tune natif, tests seulement). `build.bat` sans `asio` construit un
   moteur sans ASIO (WASAPI, nul, fichier).

L'installateur de Nova Studio (`desktop\build.bat`) appelle `native-audio\build.bat asio` si
l'exécutable manque, puis embarque `NovaAudioEngine.exe`, `LICENCES.md`, le texte de la GPLv3,
la licence MIT du SDK VST3 et `SOURCE.txt` (commit d'où le moteur a été construit) dans le dossier
`audio-engine` ; la construction s'arrête si l'un d'eux manque ou si un fichier du pilote factice
s'y trouve (`desktop/native_audio_gate.py`).

## Mode audio pro Nova (édition pro, `Source/pro/`)

Décision de Romain du 30/10/2026 : ne pas attendre la contresignature de Steinberg et faire mieux
que l'ASIO **sans aucun code de Steinberg** (hors SDK VST3 MIT). Périphérique `pro` (`PROTOCOLE.md` § 6) :

- WASAPI **exclusif** piloté par événements, à la plus petite période du périphérique
  (`GetDevicePeriod`, alignement imposé géré) ; sinon **partagé faible latence** (`IAudioClient3`) ;
  formats natifs (float 32, 24 sur 32, 24 tassés, 16) sans conversion ; deux fils MMCSS « Pro Audio »
  à priorité **critique**, aucune allocation ni verrou dans ces fils (vérifié par les tests) ;
- **assistant** `pro_calibrate` : essaie les tampons sous charge, garde le plus petit sans coupure
  + une marge, **mesure** l'aller-retour (câble ou boucle) ; la latence mesurée remplace l'annoncée
  pour caler les prises (`measured_roundtrip`) ;
- **coupures détectées** (réveil tardif, bloc rendu après l'instant de lecture, file vide,
  discontinuité de capture) et **reprise automatique** (débranchement, veille, périphérique par défaut
  changé) sans relancer Nova ;
- **agrégation** de deux cartes (entrée de l'une, sortie de l'autre) avec **correction de dérive
  d'horloge** (`Source/pro/AdaptiveResampler.*` : sinc 48 coefficients × 512 phases, régulateur PI sur
  un niveau de file estimé en continu) ;
- noms des canaux (« <carte> · Gauche »), sélection des canaux (`inmap`, `outmap`).

Tests sans aucun vrai périphérique : faux `IMMDeviceEnumerator` / `IAudioClient3` (`tests/pro/`),
`NovaAudioTests.exe pro` ; bancs `probench` (mode pro) et `asiobench` (pilote ASIO factice, édition
asio). Essai sur une vraie carte : protocole pas à pas dans `D:\1 WORK\CONTENU\nova-moteur-pro\ESSAI_REEL.md`.

## Nova Tune natif (insert `novatune`, édition pro seulement)

Depuis le 30/10/2026 (décision de Romain), Nova Tune natif est **propriétaire** et n'est compilé que
dans `NovaProAudio.exe` ; l'édition ASIO (GPLv3) refuse l'insert `novatune` et ne l'annonce pas dans
`features`. Décision du 30/10/2026 (choix C2 de l'audit des standards) : l'artiste entend Nova Tune **au casque,
sans retard, pendant la prise en ASIO**. Le correcteur de la version web de Nova Studio
(`plugins/AutoTunePlugin.tsx` : détection YIN, PSOLA, mode direct, vitesse, humanisation, Flex,
vibrato, formant, transposition, gamme, notes cibles, accord) est porté ligne à ligne dans
`Source/NovaTune.*`, avec les mêmes types (calculs en double, tampons en float aux mêmes endroits,
arrondi de JavaScript) et les mêmes réglages. Interface : `PROTOCOLE.md` § 2.3 (insert `novatune`,
commande `monitor_params`).

**Même son que la version web (mesuré, 30/10/2026)** : banc `tests/novaTuneNatif.test.ts` (suite de
métrologie : `npx vitest run --mode metro tests/novaTuneNatif.test.ts`), 6 voix réalistes à vérité
connue + 3 voix réelles, 6 réglages (mode direct robot, naturel en La mineur, Flex + vibrato +
transposition ; mode qualité robot, formant + notes cibles + voix grave, accord + transposition −12) :

| Mesure | Seuil d'équivalence | Mesuré, tampon de carte 128 (et 256) | Tampon de 64 |
|---|---|---|---|
| Écart échantillon par échantillon | — | **identique au bit près** (54/54 passages) | −6 à −25 dB (raccords décalés) |
| Hauteur de sortie, 95e centile | ≤ 1 cent (64 : ≤ 4) | 0 cent | 0,6 à 3,4 cents |
| Spectre par tiers d'octave, pire bande | ≤ 0,5 dB (64 : ≤ 2) | 0 dB | 0,02 à 1,75 dB |
| Clics (> 6 kHz, blocs de 1 ms) natif − web | ≤ 1 dB | 0 dB | ≤ 0,92 dB |
| Consonnes / souffles natif − web | ≤ 1 dB | 0 dB | — |
| Latence mesurée (voix juste) natif − web | ≤ 0,1 ms | 0 : direct 0,73 ms, qualité 27,38 ms | — |

Le calcul se fait par tranches de 128 images au plus, alignées comme les blocs de rendu du
navigateur : avec un tampon multiple de 128, c'est exactement le calcul du navigateur ; avec 32, 64
ou 96, les tranches sont plus courtes (sans latence ajoutée) et la note visée arrive quelques
dizaines d'images plus tôt ou plus tard (même hauteur à ±0,5 cent sur une voix tenue).

Latence au casque (boucle simulée, `tests/Tests.cpp`) : aller-retour du pilote **+ 28 images
(0,58 ms) à 48 kHz**, aucun bloc ajouté ; sur une voix, 0,73 ms en moyenne (au plus une période de
plus pendant une correction) ; annoncée 188 images (3,9 ms) comme la version web. Coût (banc, 10 s
de rap, mode direct, tampon 128) : ≈ 14–18 ms de calcul par seconde de son, 99,9e centile
≈ 0,15 à 0,4 ms par bloc selon la charge du PC, pour un budget de 2,67 ms (mesures : `D:\1 WORK\CONTENU\nova-tune\natif\natif_vs_web.json`).

## Contribuer

Le moteur est publié sous GPLv3. Make Music, seul auteur à ce jour, pourra aussi publier des
versions futures sous une autre licence (par exemple la licence ASIO propriétaire de Steinberg,
une fois l'accord contresigné) : les versions déjà publiées sous GPLv3 le restent pour tous ceux
qui les ont reçues. Pour garder cette possibilité, une contribution extérieure n'est intégrée
qu'avec l'accord écrit de son auteur.

Limite connue, signalée comme le recommande la FSF : le moteur sait charger des pilotes ASIO et des
modules VST3 qui ne sont généralement pas libres (permission supplémentaire de `LICENCES.md`) ; il
fonctionne aussi sans eux (WASAPI, retour direct sans effet).
