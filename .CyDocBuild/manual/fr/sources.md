---
id: sources
title: Fichiers, lecture et enregistrement
description: Garder les accès fichier dans les workers et lire les sources préparées.
section: Guides
order: 3
sourceRevision: 1
parent: overview
---

## Choisir la bonne frontière {#files}

`AudioFileReader` et `AudioFileWriter` effectuent des accès fichier et peuvent allouer. Utilisez-les sur les threads de contrôle ou de travail. Interrogez `audioFileFormats()` pour les capacités réelles de lecture/écriture, limites de canaux et encodages acceptés. Les codecs de plateforme dépendent des encodeurs installés dans l’OS ; une valeur d’énumération ne garantit pas un encodeur disponible.

Le PCM entier WAV/AIFF/AIFC/CAF suit les conventions d’échantillons normalisés. L’export entier arrondit et borne dans la plage de destination. Des métadonnées absentes restent absentes ; l’écriture par chemin remplace la destination par un temporaire terminé à la fermeture. Vérifiez les échecs de `write`, `flush` et `close`, y compris le remplacement final.

## Préparer la lecture d’un fichier {#playback}

`ReadAheadSource` confie le décodage/rééchantillonnage à son worker. Préparez et préremplissez pendant que le consommateur audio est au repos. Cette fonction prépare une source stéréo et son transport ; elle ne démarre ni la lecture ni un périphérique audio.

```cpp
#include <cyaudio/sources/ReadAheadSource.h>
#include <cyaudio/sources/AudioTransport.h>

cyaudio::Status preparePlayback(const std::filesystem::path& path,
                               cyaudio::ReadAheadSource<float>& source,
                               cyaudio::AudioTransport<float>& transport) {
    cyaudio::ReadAheadOptions options;
    options.outputRate = 48000;
    options.outputChannels = 2;
    auto status = source.prepare(path, options);
    if (status != cyaudio::Status::ok) return status;
    status = source.prefill(0, 512);
    if (status != cyaudio::Status::ok) return status;
    return transport.prepare({48000, 512, 2}, source);
}
```

Envoyez `TransportAction::play` depuis l’unique producteur de contrôle ; `AudioTransport::process` applique les commandes sur son propriétaire audio. Arrêtez le consommateur avant de fermer la source. Celle-ci vit plus longtemps que le transport. Les lectures courtes indiquent leur nombre exact de frames et complètent la fin par du silence.

`ReadAheadOptions` règle aussi la capacité des pages, les canaux/fréquence de sortie, les cartes temporelles et la politique de stretching. `SourceStretchMode` distingue choix automatique, rééchantillonnage et stretching ; consultez `stretchInfo()` plutôt que supposer le mode ou la latence. Le préremplissage peut attendre et reste hors callback. Suivez les sous-alimentations et choisissez explicitement `UnderrunPolicy` dans le transport.

## Enregistrer sans accès fichier dans le callback {#recording}

Préparez `BackgroundRecorder` avec format, encodage, taille maximale de bloc et capacité de file. Le producteur audio appelle `push` ; le bloc est accepté ou refusé en entier. Surveillez `droppedBlocks()`. Après arrêt du producteur, appelez `finish()` sur le contrôle pour vider/rejoindre le worker et finaliser le fichier. La destruction n’est pas une opération du thread audio.

Les sources mémoire, proxies PCM, caches de formes d’onde et ressources d’instruments utilisent les mêmes [buffers et conventions de propriété](doc:api#buffers).
