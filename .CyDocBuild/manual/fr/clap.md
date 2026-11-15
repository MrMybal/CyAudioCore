---
id: clap
title: Utiliser les extensions CLAP
description: Négocier les capacités stables et services de l’hôte.
section: Plugins
order: 7
sourceRevision: 1
parent: plugins
---

## Interroger les capacités {#capabilities}

Incluez `cyaudio/plugins/ClapExtensions.h` et liez `CyAudioCore::Plugins`. `PluginInstance::clapExtensions()` retourne une interface empruntée pour CLAP, et null pour les autres formats. Elle cesse d’être valide à la fermeture ou au remplacement de l’instance. L’ABI de référence est CLAP 1.2.10. Cette bibliothèque est un hôte ; elle n’exporte pas de plugins CLAP.

Vérifiez `capabilities()` avant d’appeler une extension optionnelle. Une capacité absente retourne `unsupported_format`. Les extensions stables sont négociées d’après les fonctions du plugin réel ; les extensions draft/tierces ne sont pas annoncées automatiquement et les espaces d’événements personnalisés inconnus ne sont pas traduits.

| Besoin | Intégration publique |
| --- | --- |
| Ports audio/note | Bus distincts, sidechains, IDs stables et dialectes pris en charge |
| Configurations | Énumération/configuration inactive ; cartes surround/ambisoniques |
| Paramètres | Valeurs natives, conversion texte, gestes, automation et modulation par note |
| États/presets | Flux bornés, contextes d’état, chargement et découverte de presets |
| Contexte applicatif | Informations de piste, noms de notes, voix et contrôles distants |
| Éditeurs | Négociation GUI native, tailles/DPI, titre flottant et menus contextuels |
| Ordonnancement | Mode de rendu, latence/traîne, statut de traitement et requêtes différées |

## Fournir des services applicatifs {#services}

Fournissez `ClapHostServices` via `PluginHostCallbacks::clap` avant la création de l’instance si vous voulez rendre les informations de piste ou callbacks de menu disponibles pendant l’initialisation. Gardez le contexte emprunté en vie et sérialisez ses changements. Les contrôles avancés sont copiés à travers la frontière de processus ; aucun pointeur de code applicatif n’est transféré dans le worker.

Pour les fournisseurs isolés, `PluginIsolationOptions::clapThreadPool=true` active un exécuteur de tâches **sériel dans le worker**. Il est optionnel et ne signifie pas rendu parallèle. Un callback applicatif `executeTasks` arbitraire n’est pas pris en charge à travers l’isolation. `flushAudio()` et l’activation de ports audio actifs suivent des échanges non bloquants à réessayer et peuvent retourner `would_block`.

Pompez les services main hors audio. Logs et notifications de rescan/preset sont livrés par une file bornée sur ce pompage. Les timers et services POSIX de descripteurs disponibles appartiennent au contrôle du worker ; ils ne deviennent pas des accès I/O du callback périphérique.

## Découvrir les presets {#presets}

`ClapPresetDiscovery` demande la même configuration d’isolation que les fournisseurs de plugins. Il expose fournisseurs, emplacements, soundpacks et métadonnées copiées. Il ne parcourt pas les disques arbitrairement et ne charge pas de preset à la place de l’utilisateur. Gardez découverte et opérations d’état/preset sur le contrôle et traitez extensions absentes, limites de capacité et échecs de worker.

Voir le [chargement et les états](doc:plugins#state) et l’[ordonnancement des workers](doc:workers).
