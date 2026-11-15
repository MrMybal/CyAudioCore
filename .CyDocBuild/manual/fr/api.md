---
id: api
title: Modules et buffers publics
description: Retrouver les headers publics et conventions audio communes.
section: Référence
order: 8
sourceRevision: 1
---

## Choisir les modules {#modules}

Les headers publics sont sous `include/cyaudio` et les déclarations dans le namespace `cyaudio`. `cyaudio/CyAudioCore.h` agrège les interfaces de services. Préférez un header public précis si vous avez besoin d’une frontière d’inclusion plus petite.

| Cible CMake | Rôle |
| --- | --- |
| `CyAudioCore::CyAudioCore` | Agrégation INTERFACE des modules statiques configurés |
| `CyAudioCore::Core` | Protocoles MIDI, orchestration de synthèse, sources mémoire et transport |
| `CyAudioCore::Files` | Codecs, métadonnées, proxies PCM, formes d’onde et ressources d’instruments |
| `CyAudioCore::Devices` | Périphériques audio/MIDI et gestionnaires de connexion |
| `CyAudioCore::Sources` | Lecture anticipée, intégration du stretching et enregistrement ; exige Files |
| `CyAudioCore::Plugins` | Hébergement CLAP/VST3/LV2/JSFX et workers en processus séparés |

L’agrégat est une cible INTERFACE, pas une nouvelle archive binaire combinée. Le paquet installé résout ses dépendances CyDSP et système par CMake. Les types natifs des SDK de plugins/périphériques restent privés.

## Buffers, vues et formats {#buffers}

`AudioBuffer<T>`, `AudioBlock<T>`, `ProcessSpec`, `Status`, `VectorOps` et conversions de disposition sont les types publics exacts de CyDSP, exposés par alias. `AudioBuffer` possède le stockage préparé ; `AudioBlock` emprunte des pointeurs de canaux et plages de frames. Une vue ne prolonge pas la vie du stockage. Utilisez `AudioBlock<const T>` pour une entrée en lecture seule.

Préparez avec fréquence positive, canaux et taille maximale de bloc. Respectez les dimensions des vues actives et les statuts retournés. Les callbacks périphériques restent float32 ; fichiers, sources et processeurs de plugins compatibles peuvent utiliser float ou double. Ne déduisez pas une prise en charge float64 d’un plugin float32 et ne traitez pas du stockage entrelacé comme des pointeurs de canaux planaires. Le calcul numérique et le dispatch SIMD appartiennent à CyDSP.

## Points d’entrée publics utiles {#headers}

| Header | Interfaces principales |
| --- | --- |
| `cyaudio/core/DspTypes.h` | Types CyDSP communs et alias de buffers/dispositions |
| `cyaudio/io/AudioDeviceManager.h` | `AudioDeviceManager`, reconnexion et contrats de driver |
| `cyaudio/io/AudioFile.h` | `AudioFileReader`, `AudioFileWriter`, capacités des formats |
| `cyaudio/sources/AudioTransport.h` | `AudioTransport`, commandes, tempo et position |
| `cyaudio/sources/ReadAheadSource.h` | `ReadAheadSource`, `BackgroundRecorder` |
| `cyaudio/io/MidiConnection.h` | Connexion MIDI en file, horloges et générations |
| `cyaudio/plugins/Plugin.h` | Choix de fournisseur, cycle de vie et échange audio |
| `cyaudio/plugins/ClapExtensions.h` | Contrôles CLAP stables portables et découverte de presets |
| `cyaudio/plugins/PluginWorker.h` | Helper de processus et contrôle IPC bas niveau |

Lisez les [règles de propriété et de gestion d’erreur](doc:lifecycle) avant de raccorder ces interfaces à un callback audio. Les exemples de ce manuel montrent l’usage public et ne sont pas des listings d’implémentation.
