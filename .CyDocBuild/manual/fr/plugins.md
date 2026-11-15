---
id: plugins
title: Héberger des plugins
description: Charger CLAP et choisir le cycle de vie du plugin.
section: Plugins
order: 5
sourceRevision: 1
---

## Configurer d’abord l’isolation {#load}

Les fournisseurs intégrés CLAP, VST3, LV2 et JSFX utilisent toujours des processus séparés. Les scans sont des workers jetables et chaque instance active possède son worker, éditeur inclus. La découverte de presets CLAP est également isolée. Fournissez un chemin absolu d’exécutable ; son absence retourne `invalid_argument`. Aucun échec ne charge un module tiers dans le moteur.

Compilez `cyaudio_plugin_worker` avec `CYAUDIOCORE_BUILD_PLUGIN_WORKER=ON`, ou fournissez un helper applicatif lié à `CyAudioCore::Plugins`, qui reconnaît `--cydsp-worker <mapping>` et appelle `runPluginWorker`. Son ABI et son protocole version 3 doivent correspondre à la bibliothèque.

Cette fonction de contrôle reçoit des chemins absolus et charge le premier descripteur du module choisi. Un navigateur destiné à l’utilisateur doit plutôt lui permettre de sélectionner le descripteur.

```cpp
#include <cyaudio/plugins/Plugin.h>

cyaudio::Status loadClap(const std::filesystem::path& worker,
                        const std::filesystem::path& module,
                        std::unique_ptr<cyaudio::PluginProvider>& provider,
                        cyaudio::PluginInstance& instance) {
    cyaudio::PluginIsolationOptions isolation;
    isolation.workerExecutable = worker;
    auto status = cyaudio::createPluginProvider(provider, isolation);
    if (status != cyaudio::Status::ok) return status;
    std::vector<cyaudio::PluginDescriptor> plugins;
    status = provider->scan(module, plugins);
    if (status != cyaudio::Status::ok) return status;
    if (plugins.empty()) return cyaudio::Status::unsupported_format;
    return provider->create(module, plugins.front().id, {}, instance);
}
```

La surcharge sans `PluginFormat` sélectionne CLAP. Les autres formats demandent un choix explicite, sans repli silencieux. L’interface publique n’expose aucun type de SDK de plugin. `PluginInstance::adopt` accepte des backends applicatifs de confiance ; il ne charge pas de modules tiers.

## Activer et traiter {#lifecycle}

Consultez `description()` pour préparer les bus audio et buffers d’événements. Configurez si nécessaire les ports inactifs et paramètres. Appelez `activate` avec fréquence et bornes de frames pendant que le propriétaire audio est au repos ; restez dans les capacités préparées du worker. Start/process/stop utilisent un propriétaire audio sérialisé unique. Le float32/float64 se négocie avec le plugin et ne se suppose pas.

Le [mode pipelined](doc:workers#pipelined) par défaut utilise `process()` avec la taille fixe de l’activation. Le [mode même cycle](doc:workers#same-cycle) emploie les appels séparés et demande un ordonnanceur applicatif. Appelez `pumpMainThread()` sur le propriétaire UI/contrôle pour les requêtes différées, diagnostics et travail de l’éditeur natif. Les opérations de fenêtre restent sur le propriétaire UI adapté ; l’application fournit sa boucle d’événements.

## Conserver l’état et se rétablir {#state}

Sauvegardez/chargez états et presets sur le contrôle sérialisé, avec l’audio au repos lorsque nécessaire. Vérifiez erreurs et limites de capacité. Lors d’un crash de worker, la sortie audio devient silencieuse et la santé indique une déconnexion ; l’application choisit de contourner, recréer l’instance et restaurer un état déjà sauvegardé. Aucun redémarrage en boucle ou plugin de remplacement n’est automatique.

L’isolation contient les crashs de processus. Elle ne restreint pas les privilèges OS et ne garantit pas l’échéance temps réel du code tiers. Consultez la [négociation des extensions CLAP](doc:clap) et l’[ordonnancement des workers](doc:workers).
