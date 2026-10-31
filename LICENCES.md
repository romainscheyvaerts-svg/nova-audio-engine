# NovaAudioEngine : licence et sources

`NovaAudioEngine.exe` (dossier `audio-engine` de Nova Studio installé) est le **moteur audio natif**
utilisé par Nova Studio pour la carte son à faible latence. C'est un **programme séparé** :
Nova Studio le lance et lui parle par un protocole documenté (`PROTOCOLE.md` : commandes JSON sur
l'entrée et la sortie standard, échantillons audio dans une mémoire partagée). Le reste de Nova
Studio n'est pas couvert par cette licence et reste un logiciel propriétaire de Make Music.

## Licence du moteur : GNU GPL version 3

Copyright (C) 2026 Make Music.

NovaAudioEngine est un **logiciel libre** : vous pouvez le redistribuer et/ou le modifier selon les
termes de la **GNU General Public License version 3** publiée par la Free Software Foundation
(texte complet : `LICENSE.txt` à côté de ce fichier, `native-audio/LICENSE` dans les sources,
https://www.gnu.org/licenses/gpl-3.0.html), avec la permission supplémentaire ci-dessous.

Ce programme est distribué dans l'espoir qu'il sera utile, mais **SANS AUCUNE GARANTIE**, sans même
la garantie implicite de qualité marchande ou d'adéquation à un usage particulier. Voir la GNU
General Public License pour plus de détails.

### Permission supplémentaire (article 7 de la GPLv3)

Texte qui fait foi (anglais) :

> Additional permission under GNU GPL version 3 section 7: If you modify this Program, or any
> covered work, by linking or combining it with an audio device driver (such as an ASIO driver
> supplied by a sound card manufacturer) or an audio plug-in (such as a VST3 plug-in) that is loaded
> at run time, whatever the license of that driver or plug-in, the licensors of this Program grant
> you additional permission to convey the resulting work. Corresponding Source for a non-source form
> of such a combination need not include the source code of such drivers or plug-ins.

En français : le moteur peut charger, à l'exécution, les pilotes de cartes son (pilotes ASIO des
fabricants) et les modules d'effets (VST3) installés sur l'ordinateur, quelle que soit leur licence ;
leurs sources n'ont pas à être fournies. Cette permission couvre le code dont Make Music est
l'auteur ; elle ne change rien aux licences des composants de tiers listés plus bas.

## Code source correspondant

- Sources du moteur, scripts de construction et tests :
  **https://github.com/romainscheyvaerts-svg/NOVA-STUDIO-DAW-0901-/tree/main/native-audio**
  (version exacte de l'exécutable installé : voir `SOURCE.txt` à côté de ce fichier, qui donne le
  commit Git d'où il a été construit).
- Fichiers partagés avec d'autres outils de Nova Studio et compilés dans le moteur, distribués dans
  ce cadre sous la même licence (GPLv3 + permission supplémentaire) :
  `native-host/Source/VstInstance.*`, `native-host/Source/Guard.*`, `native-host/Source/SharedAudio.h`,
  `native-common/*` (même dépôt).
- Comment obtenir les SDK de Steinberg et reconstruire l'exécutable pas à pas : `native-audio/README.md`.

### Offre écrite

Pendant au moins **trois ans** après la dernière distribution de chaque version de NovaAudioEngine,
Make Music fournit à toute personne qui possède l'exécutable, sur simple demande, une copie
**complète** de la Source correspondante de cette version (sources du moteur, fichiers partagés,
scripts de construction et parties utilisées des SDK VST3 et ASIO de Steinberg) : gratuitement par
téléchargement, ou sur un support physique habituel pour un prix qui ne dépasse pas le coût
raisonnable de la copie et de l'envoi. Demande : par une « issue » sur le dépôt GitHub ci-dessus ou
via le formulaire de contact de https://studiomakemusic.com, en précisant la version de Nova Studio.

## Composants de tiers compilés dans le moteur

| Composant | Origine | Licence |
|---|---|---|
| Sources du moteur (`native-audio/Source/*`, CMake, `build.bat`, tests) | Make Music | **GPLv3** + permission supplémentaire |
| Fichiers partagés (`native-host/Source/VstInstance.*`, `Guard.*`, `SharedAudio.h`, `native-common/*`) | Make Music | double licence : Make Music, **ou GPLv3** + permission supplémentaire (dans le moteur) |
| VST3 SDK 3.8.1 (`pluginterfaces`, `base`, `public.sdk/source/common` et `vst/hosting`) | https://github.com/steinbergmedia/vst3sdk, tag `v3.8.1_build_84` | **MIT** (Steinberg Media Technologies GmbH), compatible GPLv3 ; texte : `licences/VST3_SDK-MIT.txt` |
| ASIO SDK 2.3.4 (en-têtes `common/asio.h`, `common/iasiodrv.h`, `common/asiosys.h`, utilisés tels quels) | Steinberg, https://www.steinberg.net/asiosdk (`ASIO-SDK_2.3.4_2025-10-15.zip`) | double licence Steinberg : utilisé ici sous **GPLv3** |
| Bibliothèque d'exécution Visual C++ (liée statiquement) | Microsoft | bibliothèque système du compilateur (exception « System Libraries » de la GPLv3) |

Le pilote ASIO factice des tests (`native-audio/fake-asio/`) n'est jamais livré.

## Pilote ASIO universel livré à côté : FlexASIO (programme séparé)

Le dossier `pilotes` à côté de ce fichier contient l'installateur officiel, non modifié, de
**FlexASIO 1.10b** d'**Etienne Dechamps** (pilote ASIO universel, licence **MIT**, source :
https://github.com/dechamps/FlexASIO). Il n'est ni compilé dans le moteur ni couvert par sa licence
GPLv3 : c'est un programme distinct, installé seulement si l'utilisateur le demande dans les Réglages
audio. Notice complète, licences de ses composants (PortAudio, libsndfile sous LGPL 2.1 avec sa
source jointe, tinytoml…) : `pilotes/LICENCES.md` et `pilotes/licences/`.

## Marques

ASIO is a registered trademark of Steinberg Media Technologies GmbH.
VST is a registered trademark of Steinberg Media Technologies GmbH.
Nova Studio n'est ni affilié à Steinberg, ni approuvé par Steinberg.
