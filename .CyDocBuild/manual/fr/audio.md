---
id: audio
title: Raccorder l’audio
description: Choisir le fournisseur, le backend et le callback préparé.
section: Guides
order: 2
sourceRevision: 1
parent: overview
---

## Fournisseur et backend système {#providers}

`AudioProvider` choisit miniaudio ou PortAudio. `AudioBackend` choisit une API système telle que WASAPI, CoreAudio, ALSA ou JACK. Ce sont deux réglages distincts. Les appels par défaut utilisent miniaudio et la sélection système automatique.

PortAudio demande `CYAUDIOCORE_WITH_PORTAUDIO=ON`. Sa recette statique active WASAPI/WinMM sous Windows, CoreAudio sous macOS et ALSA sous Unix desktop ; JACK est une option Unix explicite avec sa dépendance de développement. Cette recette n’implémente pas ASIO et n’embarque pas son SDK. Android et WebAudio ne sont pas couverts par ce fournisseur PortAudio. Un fournisseur/backend explicitement indisponible provoque une erreur, sans changement silencieux de choix.

## Ouvrir et produire l’audio {#callback}

Appelez cette fonction depuis le propriétaire de contrôle sérialisé, avec un fournisseur/backend disponible dans votre build. Elle ouvre et démarre une sortie silencieuse. L’adaptateur prépare le silence avant d’appeler le callback ; remplacez son corps par votre moteur préparé. Conservez le gestionnaire jusqu’à sa fermeture.

```cpp
#include <cyaudio/io/AudioDeviceManager.h>

cyaudio::Status openSilentAudio(cyaudio::AudioDeviceManager& audio,
                              cyaudio::AudioProvider provider,
                              cyaudio::AudioBackend backend) {
    cyaudio::AudioContextOptions options;
    options.provider = provider;
    options.backend = backend;
    auto status = audio.prepare(options);
    if (status != cyaudio::Status::ok) return status;
    cyaudio::AudioDeviceConfig config;
    config.sampleRate = 48000;
    config.outputChannels = 2;
    config.maxCallbackFrames = 512;
    return audio.openWithContext(config,
        [](void*, cyaudio::AudioBlock<const float>,
           cyaudio::AudioBlock<float>, const cyaudio::AudioCallbackContext&) noexcept {
            return cyaudio::Status::ok;
        }, nullptr);
}
```

Les callbacks utilisent des vues `AudioBlock` float32. L’adaptateur privé gère les dispositions natives et découpe les blocs dépassant `maxCallbackFrames` dans la capacité préparée. Une entrée absente devient du silence. `activeInputChannels` et `activeOutputChannels` sélectionnent les canaux physiques ; leur ordre fixe celui des canaux applicatifs. Les sorties non sélectionnées restent silencieuses.

## Reconnecter sur le propriétaire de contrôle {#reconnect}

Appelez régulièrement `poll(nowMs)` hors du thread audio. `AudioDeviceManager` ne possède pas de thread d’interrogation caché. Traitez ses callbacks et `lastError()`. La reconnexion par défaut n’autorise pas le repli vers un autre périphérique par défaut ; ce choix appartient explicitement à `AudioReconnectOptions`.

Les identifiants sont propres au fournisseur. Conservez les IDs miniaudio. Un index d’énumération PortAudio n’est pas une identité matérielle persistante ; les identités dupliquées peuvent n’être valables que pour leur instantané de catalogue et les résolutions ambiguës sont refusées. Énumérez de nouveau plutôt que deviner un remplacement.

## Horloges et capacités {#timing}

Utilisez `AudioCallbackContext` et la génération du flux pour relier frames et horodatages. Une nouvelle génération repart à zéro pour les frames. Les valeurs temporelles portent un drapeau `valid` et une source de mesure ; les structures de capacités indiquent quels champs sont connus. Préservez les valeurs inconnues. Ne réinterprétez pas une horloge de backend comme une date `steady_clock` et ne déduisez pas des formats natifs à partir de réglages suggérés.

Arrêtez/fermez depuis le propriétaire de contrôle avant de détruire l’état emprunté par le callback. Voir les [durées de vie et erreurs](doc:lifecycle).
