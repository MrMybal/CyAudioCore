---
id: lifecycle
title: Durées de vie et erreurs
description: Préparer les ressources et séparer contrôle et propriétaire audio.
section: Référence
order: 9
sourceRevision: 1
parent: api
---

## Séparer les propriétaires {#owners}

| Contrôle/UI ou worker | Propriétaire audio sérialisé |
| --- | --- |
| Allouer/préparer ; énumérer/ouvrir/fermer les périphériques | Rendre les blocs préparés et échanger des événements bornés |
| Charger les fichiers, scanner les plugins, sauvegarder/charger l’état | Appliquer commandes de transport et événements de paramètres préparés |
| Activer/désactiver les plugins avec audio au repos | Gérer start/process/stop actif selon le contrat |
| Interroger les gestionnaires, pomper les requêtes main et gérer les éditeurs | Soumettre/collecter/abandonner la requête de worker courante |
| Rejoindre les workers et détruire les ressources après arrêt audio | Réinitialiser les processeurs lorsque leur contrat public le permet |

La préparation peut allouer et échouer. Le chemin audio Core préparé n’alloue/libère pas, ne verrouille pas, n’attend pas, ne journalise pas et n’accède pas aux fichiers. Ne redimensionnez pas les buffers, ne reconstruisez pas le graphe, ne rejoignez pas un worker et ne détruisez pas de ressources empruntées dans le callback. Plugins et scripts tiers n’héritent pas de cette garantie ; l’isolation ne change pas leurs algorithmes.

Gardez en vie sources, stockage d’échantillons/tables, contextes de nœuds du graphe, contextes de callback et services d’hôte pour tous leurs utilisateurs emprunteurs. Sérialisez les changements de cycle de vie avec les propriétaires indiqués par chaque API. Avant remplacement d’une source, fermeture d’un périphérique ou destruction d’un plugin, arrêtez ses accès audio et terminez le travail de contrôle en cours. Une file SPSC a un producteur et un consommateur uniques.

## Traiter le statut retourné {#errors}

| Statut | Réponse applicative |
| --- | --- |
| `not_prepared` | Préparer l’objet avant l’opération |
| `invalid_argument`, `channel_mismatch`, `block_too_large` | Corriger demande, disposition ou capacité préparée |
| `unsupported_backend`, `unsupported_format` | Choisir explicitement une implémentation ou capacité prise en charge |
| `would_block`, `transition_in_progress` | Respecter l’état non bloquant et l’échéance ; ne pas attendre en audio |
| `end_of_stream` | Examiner le nombre exact de frames et le complément silencieux prévu |
| `disconnected`, `timed_out`, `io_error` | Appliquer la stratégie de rétablissement sur le contrôle |
| `allocation_failed` | Signaler l’échec de préparation et conserver l’état valide lorsque promis |

Le comportement d’échec dépend de l’opération. De nombreuses préparations de contrôle préservent l’objet valide précédent ; cela ne promet pas d’annuler de l’audio déjà traité ou des événements émis. Les chemins périphérique/plugin fournissent le silence documenté en cas de sortie manquante/échouée. Ne supposez pas que tout appel invalide peut écrire sans risque dans une vue de sortie invalide.

## Garder les époques et temps explicites {#epochs}

Ne mélangez pas les horloges de frames entre générations de flux ou de connexion MIDI. Préservez les drapeaux de temps/capacités inconnus au lieu de les remplacer par une estimation inventée. Gérez latences native/pipeline et traînes finies/infinies/inconnues des plugins dans votre graphe. Pour les échéances, voir l’[ordonnancement dans le même cycle](doc:workers#same-cycle).
