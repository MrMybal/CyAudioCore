---
id: overview
title: CyAudioCore
description: Choisir les services de votre application audio.
section: Guides
order: 0
sourceRevision: 1
---

## Construire une application audio {#purpose}

CyAudioCore 0.1.0 est une bibliothèque C++20 pour les moteurs audio, stations de travail et applications musicales. Elle fournit les périphériques audio et MIDI, fichiers, transport, instruments et hôtes de plugins. CyDSP fournit le calcul numérique. Votre application possède son interface graphique, sa boucle d’événements et son ordonnanceur audio.

```diagram
{"type":"flow","title":"Responsabilités des bibliothèques","nodes":[{"id":"app","label":"Application","x":0,"y":0},{"id":"core","label":"Services CyAudioCore","x":260,"y":0},{"id":"dsp","label":"Traitements CyDSP","x":520,"y":0}],"edges":[{"from":"app","to":"core"},{"from":"core","to":"dsp"}]}
```

Cette version est **en développement** et distribuée sous forme de sources. Compilez des archives statiques correspondant à l’ABI de votre application. La couverture d’exécution actuelle de Core porte sur Windows x64 ; les périphériques matériels et plugins installés doivent être évalués dans votre application. Cette couverture ne valide pas les autres cibles desktop et mobiles.

## Choisir un point de départ {#start}

- [Compiler et lier](doc:setup) : utiliser le paquet CMake et rendre un bloc en mémoire.
- [Raccorder l’audio](doc:audio) : choisir miniaudio ou PortAudio et un backend système.
- [Lire et enregistrer des fichiers](doc:sources) : garder les accès fichier hors du callback audio.
- [Raccorder le MIDI](doc:midi) : collecter les événements horodatés et gérer les reconnexions.
- [Héberger des plugins](doc:plugins) : configurer l’isolation obligatoire et CLAP.
- [API publique et durées de vie](doc:api) : choisir les modules et respecter les propriétaires.

## Utiliser ce manuel {#navigation}

Ouvrez `Documentation.html` avec le dossier `DocData` complet à côté. La lecture ne demande ni serveur ni Node.js. Utilisez l’arbre des pages, le sommaire, le choix de langue et la recherche ; Ctrl+K ouvre la recherche. Les pages et sections anglaises et françaises partagent leurs identifiants : un lien contextuel peut changer de langue en gardant sa destination.
