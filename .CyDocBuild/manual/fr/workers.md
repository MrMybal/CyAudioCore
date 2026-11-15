---
id: workers
title: Ordonnancer les workers de plugins
description: Choisir le pipeline ou la soumission non bloquante dans le même cycle.
section: Plugins
order: 6
sourceRevision: 1
parent: plugins
---

## Capacités préparées {#capacity}

Réglez `PluginIsolationOptions` avant de créer le fournisseur. Les limites par défaut sont 8192 frames, 4096 événements, 64 canaux par direction, 64 Kio de payload d’événements et une enveloppe contrôle/état de 16 Mio. Les bornes d’activation doivent tenir dans ces ressources préparées. Les gros états incluent aussi le surcoût de sérialisation ; un payload exactement égal à l’enveloppe n’est pas garanti compatible.

Le stockage audio partagé et les ressources de contrôle sont préparés hors traitement. Le chemin hôte préparé n’alloue/libère pas, n’attend pas et n’interroge pas la santé du processus. Il peut émettre un réveil non bloquant quand le propriétaire audio enfant dort. Le code du plugin dans le worker conserve son propre comportement.

## Traitement pipelined {#pipelined}

`PluginScheduling::pipelined` est le choix par défaut. `process()` soumet le bloc courant et retourne le bloc précédent terminé ; la première sortie est silencieuse. Sa taille de bloc est le `maximumFrames` de l’activation, même si la capacité de la mémoire partagée est supérieure. Chaque frontière de worker en série ajoute un bloc de pipeline.

`PluginInstance::timing()` inclut le pipeline lorsque la latence native est connue. Une valeur native inconnue reste inconnue ; `PluginWorkerInfo` bas niveau sépare valeurs natives et pipeline. Tenez compte de ces informations dans la compensation de latence du graphe.

## Traitement dans le même cycle {#same-cycle}

Sélectionnez `PluginScheduling::sameCycle` avant de créer le fournisseur. Utilisez des tailles variables dans les bornes d’activation et un propriétaire audio sérialisé unique :

1. Appelez `submit(block, ticket)` pour copier audio/événements dans la mémoire partagée préparée. Une seule requête peut être en vol.
2. Exécutez d’autres nœuds prêts du graphe applicatif.
3. Appelez `collect(block, ticket)` pour ce même ticket. `would_block` indique que le résultat manque et rend la sortie silencieuse. Ne réessayez que s’il reste du travail indépendant utile et du temps avant l’échéance.
4. À l’échéance, appelez `discard(ticket)`. Un résultat tardif n’est jamais réutilisé comme bloc suivant. Une nouvelle soumission peut rester `would_block` jusqu’à la fin du travail abandonné.

Ces appels n’attendent jamais. N’ajoutez ni boucle active ni sommeil autour de `collect` dans un callback périphérique. Le `process()` classique est invalide dans ce mode. Aucun retard fixe de pipeline n’est ajouté, mais le plugin peut manquer son échéance ; la bibliothèque ne fournit pas l’ordonnanceur de graphe de l’application.

## Contrôle et échec {#control}

Exécutez activation, RPC d’état, opérations d’éditeur et contrôles de santé hors callback. Les appels de contrôle en lecture ne consomment pas de ticket audio. Arrêtez les accès audio avant de changer les dispositions ou détruire/remplacer l’instance. Lors d’un blocage ou crash, choisissez explicitement la politique silence/contournement/rétablissement du moteur. Voir les [contrats de cycle de vie](doc:lifecycle).
