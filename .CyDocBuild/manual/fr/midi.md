---
id: midi
title: MIDI et instruments
description: Collecter les événements horodatés et gérer les générations de connexion.
section: Guides
order: 4
sourceRevision: 1
parent: overview
---

## Choisir un transport {#transports}

Utilisez `MidiInput`/`MidiOutput` pour les ports MIDI 1 bas niveau, ou `MidiConnection` pour les entrées/sorties en file et la reconnexion. Le MIDI 2 utilise les services natifs de points de terminaison disponibles sur chaque plateforme. MIDI-CI, profils et échange de propriétés sont des protocoles applicatifs au-dessus du transport ; leur prise en charge ne prouve pas qu’un périphérique donné les implémente.

Énumérez sur le propriétaire de contrôle et résolvez un `MidiPortSelector` dans le catalogue courant. Les index sont des instantanés. Persistez une identité prise en charge et n’autorisez le repli par nom que volontairement. Les IDs/noms dupliqués sont refusés lorsqu’ils sont ambigus. Ne déduisez pas l’identité d’une ancienne position d’énumération.

## Transférer les événements à l’audio {#events}

1. Préparez les buffers MIDI/UMP bornés et les files de connexion avant la lecture.
2. Appelez `poll(nowMs)` sur un propriétaire de contrôle/travail sérialisé. Aucun thread d’interrogation caché n’est créé.
3. Sur le propriétaire audio, appelez `collect` avec l’`AudioClockAnchor` courant et la longueur du bloc. Les événements portent des offsets de frames dans le bloc de destination.
4. Fournissez les événements préparés à l’instrument ou au plugin avec l’audio et le contexte de transport de ce bloc.

`MidiOutput::send` direct peut bloquer, notamment pour le SysEx, et appartient à un worker. La file de sortie préparée de `MidiConnection` offre une frontière distincte pour le producteur audio. Gardez des capacités explicites ; consultez les compteurs de pertes/dépassements. En entrée bas niveau, un buffer trop petit ne consomme pas le message.

## Reconnecter et relâcher les voix {#reconnect}

`MidiConnection::generation()` identifie les époques de connexion. Après une déconnexion, les anciens événements sont écartés. Relâchez les notes maintenues quand l’état connecté ou la génération change ; attendre les note-off d’un port perdu peut laisser des voix bloquées. Sérialisez ouverture/fermeture/poll hors du thread audio.

## Jouer un instrument {#instruments}

Préparez `Synthesizer<float>` ou `Synthesizer<double>` avec fréquence, taille maximale de bloc, canaux et nombre de voix. Fournissez les événements MIDI/UMP à leur offset de frame. Configurez les ressources d’oscillateur, wavetable, échantillon ou rendu personnalisé avant utilisation et gardez les tables/données d’échantillon empruntées en vie. CyDSP traite générateurs, enveloppes et rendu numérique ; Core orchestre les voix et le comportement MIDI.

Utilisez les [cibles publiques des modules](doc:api#modules) et respectez les [durées de vie de l’audio préparé](doc:lifecycle#owners).
