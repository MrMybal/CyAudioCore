<table width="100%">
  <tr>
    <td width="300" align="left" valign="top">
      <a href="docs/Modules.md">Documentation</a><br>
      <a href="docs/Migration.fr.md">Migration</a><br><br>
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

Framework audio C++20 indépendant de toute interface graphique. Utilise CyDSP pour le calcul et fournit les périphériques, fichiers, MIDI, transport, instruments et hôtes de plugins.

> **EN DÉVELOPPEMENT · 0.1.0**
>
> Sources uniquement ; aucune publication binaire. Consulter les [notes de version](VERSION.fr.md).

## À propos de CyAudioCore

L’application choisit son interface et peut utiliser CyUI. Les buffers, types et traitements optimisés sont ceux de CyDSP ; les SDK de plugins et codecs restent privés. Le worker de plugins appartient à CyAudioCore.

## Fonctionnalités

- Audio/MIDI : périphériques, reconnexion, horloges, MIDI1/MPE/UMP/MIDI2/MIDI-CI.
- Fichiers : codecs, métadonnées, proxy PCM, cache de formes d’onde et instruments.
- Sources : lecture en arrière-plan, transport et enregistrement.
- Plugins : CLAP/VST3/LV2/JSFX, paramètres, états, éditeurs natifs et isolation en processus.

---

## Installation

Compiler les archives pour l’ABI cible avec CMake 3.24+ et un compilateur C++20. CyAudioCore exige CyDSP 0.3.0. Voir le [guide de migration](docs/Migration.fr.md).

Version CyDSP actuellement validée : 0.3.0. Voir la [mise à jour de la dépendance et des archives](docs/CyDSPCurrent.fr.md).

```cmake
find_package(CyAudioCore 0.1 CONFIG REQUIRED)
target_link_libraries(MyApplication PRIVATE CyAudioCore::CyAudioCore)
```

## Sources, compilation et bibliothèques nécessaires

```sh
cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64 -DCYAUDIOCORE_CYDSP_SOURCE_DIR=../CyDSP
cmake --build build/release --config Release
cmake --install build/release --config Release --prefix build/install
```

Les tests et mesures sont désactivés par défaut. Le script scripts/run_native.py les active explicitement sous Windows ; --sanitize active AddressSanitizer. Les backends de plateforme exigent leurs SDK système. Aucun téléchargement implicite.

---

Les périphériques utilisent miniaudio par défaut. `CYAUDIOCORE_WITH_PORTAUDIO=ON` ajoute un fournisseur statique derrière les mêmes API. `AudioProvider` choisit le fournisseur et `AudioBackend` l’API système. Voir [options, exemple et limites](docs/PortAudio.fr.md).

## Licence

CyAudioCore utilise la [licence MIT](LICENSE). Les dépendances conservent leurs propres licences ; les notices complètes sont dans [Licensing.md](docs/Licensing.md).

| Bibliothèque · sources | Licence · texte | Utilisation / périmètre |
| --- | --- | --- |
| [CyDSP](https://github.com/MrMybal/CyDSP) | MIT | Dépendance numérique séparée ; dépôt privé |
| [miniaudio](https://github.com/mackron/miniaudio) | [MIT-0](third_party/licenses/miniaudio.txt) | Backend privé des périphériques audio |
| [PortAudio](https://github.com/PortAudio/portaudio) | [MIT](third_party/licenses/portaudio.txt) | Fournisseur privé d’E/S audio statique, optionnel |
| [rtmidi](https://github.com/thestk/rtmidi) | [MIT](third_party/licenses/rtmidi.txt) | Backend privé des ports MIDI1 |
| [choc](https://github.com/Tracktion/choc) | [ISC AND BSD-3-Clause AND LicenseRef-minimp3-Public-Domain](third_party/licenses/choc.txt) | Codecs audio privés et leurs sous-composants |
| [windows-midi](https://github.com/microsoft/MIDI) | [MIT](third_party/licenses/windows-midi.txt) | Déclarations natives privées et builders MIDI2 |
| [clap](https://github.com/free-audio/clap) | [MIT](third_party/licenses/clap.txt) | Déclarations privées pour l’hôte CLAP |
| [vst3-interfaces](https://github.com/steinbergmedia/vst3_pluginterfaces) | [MIT](third_party/licenses/vst3-interfaces.txt) | Déclarations privées pour l’hôte VST3 |
| [lv2](https://github.com/lv2/lv2) | [ISC](third_party/licenses/lv2.txt) | Déclarations privées LV2 |
| [lilv](https://github.com/lv2/lilv) | [ISC](third_party/licenses/lilv.txt) | Découverte et instanciation LV2 privées |
| [serd](https://github.com/drobilla/serd) | [ISC](third_party/licenses/serd.txt) | Analyse RDF/Turtle privée pour LV2 |
| [sord](https://github.com/drobilla/sord) | [ISC](third_party/licenses/sord.txt) | Index RDF privé pour LV2 |
| [sratom](https://github.com/lv2/sratom) | [ISC](third_party/licenses/sratom.txt) | Sérialisation Atom/RDF privée pour LV2 |
| [zix](https://github.com/drobilla/zix) | [ISC](third_party/licenses/zix.txt) | Primitives privées utilisées par LV2 |
| [ysfx](https://github.com/jpcima/ysfx) | [Apache-2.0 AND Zlib AND MIT AND ISC AND (GPL-2.0-or-later WITH Bison-exception-2.2)](third_party/licenses/ysfx.txt) | Runtime JSFX/EEL2 privé et dépendances avec leurs exceptions |

## Références et liens

- [Documentation](docs/Modules.md)
- [Architecture](docs/Architecture.md)
- [Migration](docs/Migration.fr.md)
