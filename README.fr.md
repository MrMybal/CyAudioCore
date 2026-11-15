<table width="100%">
  <tr>
    <td width="300" align="left" valign="top">
      <a href="https://mrmybal.github.io/CyAudioCore/#/fr/overview">Manuel</a><br>
      <a href="https://github.com/MrMybal/CyAudioCore">Dépôt</a><br><br>
      <sub>README</sub><br>
      <a href="README.md">English</a> · <a href="README.fr.md">Français</a>
    </td>
    <td width="400" align="center" valign="middle"><strong>CyAudioCore</strong><br><sub>C++20 audio</sub></td>
    <td width="300" align="right" valign="top">
      <p><sub>Licence de la bibliothèque</sub><br><a href="LICENSE"><strong>MIT</strong></a></p>
      <p><sub>Version</sub><br><a href="VERSION.fr.md"><strong>0.1.0</strong></a><br><strong>EN DÉVELOPPEMENT</strong></p>
    </td>
  </tr>
</table>

<h1 align="center">CyAudioCore</h1>

Bibliothèque audio C++20 pour les développeurs de moteurs audio, de stations de travail et d’applications musicales. Elle fournit les périphériques, fichiers, MIDI, transport, instruments et hôtes de plugins, avec CyDSP pour le calcul numérique.

> **EN DÉVELOPPEMENT · 0.1.0**
>
> Disponible sous forme de sources. Consulter les [changements et limites connus](VERSION.fr.md) avant l’intégration.

## À propos de CyAudioCore

Assemblez un flux audio depuis une source en mémoire ou un fichier, pilotez sa lecture et son enregistrement, raccordez les entrées MIDI et hébergez des plugins. Les modules se lient comme des bibliothèques statiques ; l’application choisit son interface graphique et son ordonnanceur.

Les buffers, vues et traitements numériques sont ceux de CyDSP. Les SDK de périphériques, codecs et plugins restent privés. Préparez les ressources sur le thread de contrôle avant le traitement audio ; conservez les objets et buffers empruntés pendant toute leur utilisation.

## Fonctionnalités

- **Périphériques audio** : miniaudio par défaut ; PortAudio optionnel derrière les mêmes API. Le fournisseur d’E/S et le backend système se sélectionnent séparément.
- **MIDI** : périphériques et reconnexion, MIDI 1, MPE, UMP/MIDI 2, MIDI-CI et échange de propriétés.
- **Fichiers et sources** : codecs, métadonnées, proxy PCM pour l’édition, cache de formes d’onde, lecture anticipée et enregistrement en arrière-plan.
- **Transport et instruments** : lecture, pause, recherche, boucles, tempo, synthèse polyphonique et orchestration des voix CyDSP.
- **Plugins** : CLAP principal, VST3/LV2/JSFX sur choix explicite ; paramètres, états, presets et éditeurs natifs. Veille automatique CLAP et effets VST3 compatibles ; politiques explicites pour les autres processeurs.
- **Isolation** : scans et instances de plugins dans des processus séparés. Le mode pipelined ajoute un bloc ; le mode même cycle utilise `submit`/`collect`/`discard` avec un ordonnanceur applicatif.

---

## Installation

CyAudioCore demande CMake 3.24+, un compilateur C++20 et CyDSP 0.3.0 minimum dans la branche 0.3 ; la révision courante utilisée est 0.3.2. Les headers et archives doivent être reconstruits ensemble pour l’ABI cible. La validation actuelle de Core porte sur Windows x64 ; les autres plateformes et limites matérielles sont précisées dans les [notes de version](VERSION.fr.md).

Avec les paquets CyAudioCore et CyDSP installés dans le même préfixe, ajoutez ceci au `CMakeLists.txt` de votre application :

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyAudioApp LANGUAGES CXX)

find_package(CyAudioCore 0.1 CONFIG REQUIRED)
add_executable(MyAudioApp main.cpp)
target_link_libraries(MyAudioApp PRIVATE CyAudioCore::CyAudioCore)
```

Fournissez le préfixe d’installation avec `CMAKE_PREFIX_PATH`. Vous pouvez aussi lier uniquement les cibles nécessaires : `CyAudioCore::Core`, `Files`, `Devices`, `Sources` ou `Plugins`. Voir les [modules](docs/Modules.md).

## Sources, compilation et bibliothèques nécessaires

Les sources sont dans le [dépôt CyAudioCore](https://github.com/MrMybal/CyAudioCore). Fournissez séparément un checkout de [CyDSP](https://github.com/MrMybal/CyDSP), dont l’accès nécessite les droits sur ce dépôt privé. Cet exemple Windows utilise deux dossiers voisins :

```sh
cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64 -DCYAUDIOCORE_CYDSP_SOURCE_DIR=../CyDSP -DCYAUDIOCORE_BUILD_PLUGIN_WORKER=ON
cmake --build build/release --config Release
cmake --install build/release --config Release --prefix build/install
```

La configuration ne télécharge aucune dépendance. Les bibliothèques de production sont statiques ; les tests, mesures et vérifications de compilation sont désactivés par défaut. Les backends natifs demandent leurs SDK système.

| Option CMake | Défaut | Utilisation |
| --- | --- | --- |
| `CYAUDIOCORE_CYDSP_SOURCE_DIR` | Vide | Checkout CyDSP explicite ; sinon paquet installé |
| `CYAUDIOCORE_WITH_PORTAUDIO` | OFF | Ajoute le fournisseur PortAudio à `Devices` |
| `CYAUDIOCORE_BUILD_PLUGIN_WORKER` | OFF | Construit et installe l’exécutable `cyaudio_plugin_worker` |
| `CYAUDIOCORE_BUILD_FILES`, `CYAUDIOCORE_BUILD_DEVICES`, `CYAUDIOCORE_BUILD_SOURCES`, `CYAUDIOCORE_BUILD_PLUGINS` | ON | Active les modules correspondants |

Les fournisseurs intégrés de plugins exigent un chemin absolu de worker dans `PluginIsolationOptions`. Le worker doit correspondre à l’ABI et au protocole de la bibliothèque. L’application gère les échéances audio ; un processus séparé isole les plantages sans garantir le temps d’exécution du code tiers.

Voir la [sélection miniaudio/PortAudio et les dépendances des backends](https://mrmybal.github.io/CyAudioCore/#/fr/audio/providers), les [contrats CLAP](https://mrmybal.github.io/CyAudioCore/#/fr/clap) et la [configuration des workers](https://mrmybal.github.io/CyAudioCore/#/fr/workers).

---

## Licence

CyAudioCore utilise la [licence MIT](LICENSE). Les dépendances conservent leurs propres conditions ; gardez les notices complètes lors de la distribution. Le [guide des licences](docs/Licensing.md) et le [manifeste de provenance](third_party/manifest.json) détaillent les composants et leurs révisions.

| Bibliothèque · sources | Licence · texte | Utilisation / périmètre |
| --- | --- | --- |
| [CyDSP](https://github.com/MrMybal/CyDSP) | MIT | Dépendance numérique séparée, nécessaire à tous les modules. |
| [miniaudio](https://github.com/mackron/miniaudio) | [MIT-0](third_party/licenses/miniaudio.txt) | Fournisseur privé d’entrées/sorties audio utilisé par défaut. |
| [PortAudio](https://github.com/PortAudio/portaudio) | [MIT](third_party/licenses/portaudio.txt) | Fournisseur privé d’entrées/sorties audio statique, optionnel. |
| [rtmidi](https://github.com/thestk/rtmidi) | [MIT](third_party/licenses/rtmidi.txt) | Backend privé des périphériques MIDI 1. |
| [choc](https://github.com/Tracktion/choc) | [ISC AND BSD-3-Clause AND LicenseRef-minimp3-Public-Domain](third_party/licenses/choc.txt) | Codecs audio privés et composants permissifs embarqués. |
| [windows-midi](https://github.com/microsoft/MIDI) | [MIT](third_party/licenses/windows-midi.txt) | Déclarations privées Windows MIDI Services et builders MIDI 2 portables. |
| [clap](https://github.com/free-audio/clap) | [MIT](third_party/licenses/clap.txt) | Interfaces privées CLAP 1.2.10 ; hôte de plugins principal. |
| [vst3-interfaces](https://github.com/steinbergmedia/vst3_pluginterfaces) | [MIT](third_party/licenses/vst3-interfaces.txt) | Interfaces privées VST3 et implémentation ABI minimale. |
| [lv2](https://github.com/lv2/lv2) | [ISC](third_party/licenses/lv2.txt) | Interfaces privées des plugins LV2. |
| [lilv](https://github.com/lv2/lilv) | [ISC](third_party/licenses/lilv.txt) | Découverte et hébergement LV2 privés. |
| [serd](https://github.com/drobilla/serd) | [ISC](third_party/licenses/serd.txt) | Analyse RDF/Turtle privée pour LV2. |
| [sord](https://github.com/drobilla/sord) | [ISC](third_party/licenses/sord.txt) | Indexation RDF privée pour LV2. |
| [sratom](https://github.com/lv2/sratom) | [ISC](third_party/licenses/sratom.txt) | Sérialisation Atom/RDF privée pour LV2. |
| [zix](https://github.com/drobilla/zix) | [ISC](third_party/licenses/zix.txt) | Primitives privées utilisées par LV2. |
| [ysfx](https://github.com/jpcima/ysfx) | [Apache-2.0 AND Zlib AND MIT AND ISC AND (GPL-2.0-or-later WITH Bison-exception-2.2)](third_party/licenses/ysfx.txt) | Runtime privé JSFX/EEL2 ; les composants embarqués et l’exception du parseur Bison conservent leurs notices complètes. |
| [CyDoc](https://github.com/MrMybal/CyDoc) | [MIT](Documentation/DocData/assets/licenses/CyDoc.txt) | Lecteur de documentation et génération uniquement ; indépendant du runtime audio. |

## Références et liens

- [Manuel](https://mrmybal.github.io/CyAudioCore/#/fr/overview)
- [Architecture](docs/Architecture.md)
- [Dépôt des sources](https://github.com/MrMybal/CyAudioCore)
- [Suivi des problèmes](https://github.com/MrMybal/CyAudioCore/issues)
