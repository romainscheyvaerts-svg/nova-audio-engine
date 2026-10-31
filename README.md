# NovaAudioEngine

Moteur audio natif de Nova Studio, **logiciel libre sous GNU GPL version 3** (`LICENSE`, avec la
permission supplémentaire de `LICENCES.md`). Programme Windows autonome, écrit en C++17 **sans
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
| `Source/Resampler.*`, `Source/WavFile.*` | rééchantillonneur polyphase, fichiers WAV |
| `tests/Tests.cpp` | tests unitaires (console, aucun son réel) |
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
   et le pilote factice `NovaFakeAsio.dll` (tests seulement). `build.bat` sans `asio` construit un
   moteur sans ASIO (WASAPI, nul, fichier).

L'installateur de Nova Studio (`desktop\build.bat`) appelle `native-audio\build.bat asio` si
l'exécutable manque, puis embarque `NovaAudioEngine.exe`, `LICENCES.md`, le texte de la GPLv3,
la licence MIT du SDK VST3 et `SOURCE.txt` (commit d'où le moteur a été construit) dans le dossier
`audio-engine` ; la construction s'arrête si l'un d'eux manque ou si un fichier du pilote factice
s'y trouve (`desktop/native_audio_gate.py`).

## Contribuer

Le moteur est publié sous GPLv3. Make Music, seul auteur à ce jour, pourra aussi publier des
versions futures sous une autre licence (par exemple la licence ASIO propriétaire de Steinberg,
une fois l'accord contresigné) : les versions déjà publiées sous GPLv3 le restent pour tous ceux
qui les ont reçues. Pour garder cette possibilité, une contribution extérieure n'est intégrée
qu'avec l'accord écrit de son auteur.

Limite connue, signalée comme le recommande la FSF : le moteur sait charger des pilotes ASIO et des
modules VST3 qui ne sont généralement pas libres (permission supplémentaire de `LICENCES.md`) ; il
fonctionne aussi sans eux (WASAPI, retour direct sans effet).
