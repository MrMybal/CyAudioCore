# Hébergement des plugins en processus séparés

Les fournisseurs publics intégrés de CyAudioCore hébergent systématiquement CLAP,
VST3, LV2 et JSFX dans un worker. Un scan lance un processus jetable ; une instance
possède son propre processus, y compris son éditeur. La découverte CLAP des presets
utilise aussi un processus séparé. Aucun échec ne provoque un chargement dans le moteur.

`PluginInstance::adopt()` reste une frontière pour du code applicatif de confiance.
Il ne sert pas à charger un module de plugin. Les adaptateurs locaux utilisés par le
worker et leurs tests ne constituent plus un fournisseur public de chargement direct.
Les applications déjà compilées doivent reconstruire leurs archives et leur worker.

## Configuration et exécutable

Le chemin absolu du worker est obligatoire. Sans ce chemin,
`createPluginProvider()` renvoie `invalid_argument` et conserve le fournisseur précédent.
Le fournisseur ne lance rien à sa construction ; `scan()` et `create()` peuvent ensuite
renvoyer `io_error`, `disconnected`, `timed_out`, une erreur de format ou de préparation.
Les listes et instances de sortie restent inchangées après un échec de création/scan.

```cpp
#include <cyaudio/plugins/ClapExtensions.h>

cyaudio::PluginIsolationOptions isolation;
isolation.workerExecutable = absoluteWorkerPath;
isolation.scheduling = cyaudio::PluginScheduling::sameCycle;
std::unique_ptr<cyaudio::PluginProvider> provider;
auto status = cyaudio::createPluginProvider(provider, isolation); // CLAP
if (status != cyaudio::Status::ok) return status;
std::vector<cyaudio::PluginDescriptor> plugins;
status = provider->scan(absoluteModulePath, plugins); // Processus jetable
if (status != cyaudio::Status::ok) return status;
if (plugins.empty()) return cyaudio::Status::unsupported_format;
cyaudio::PluginInstance plugin;
status = provider->create(absoluteModulePath, plugins.front().id, {}, plugin);
if (status != cyaudio::Status::ok) return status;
// plugin.isolated() == true ; description et contrôles sont des copies/proxies.
cyaudio::PluginPrepareOptions prepared;
prepared.maximumFrames = 128;
status = plugin.activate(prepared); // Contrôle, propriétaire audio au repos
```

`maximumFrames` (8192 par défaut), `maximumEvents` (4096), canaux (64 par direction),
payload (64 Kio) et enveloppe de contrôle/état (16 Mio) bornent la mémoire partagée.
L'activation doit respecter ces capacités ; elle ne relance pas le plugin et conserve
son état. Le float32 et le float64 négociés utilisent la même capacité préparée.
La limite de l'enveloppe inclut les données de sérialisation : un état occupant
exactement toute cette enveloppe peut être refusé avec `block_too_large`.

Deux possibilités pour fournir le worker :

- Compiler son propre petit exécutable lié à `CyAudioCore::Plugins`, reconnaissant
  exclusivement `--cydsp-worker <mapping>` et appelant `runPluginWorker(mapping)`.
  Voir [PluginWorkerMain.cpp](../examples/PluginWorkerMain.cpp).
- Activer explicitement `CYAUDIOCORE_BUILD_PLUGIN_WORKER=ON`. CMake construit
  `cyaudio_plugin_worker` et l'installe dans `bin`. L'option est OFF par défaut ;
  les bibliothèques de production restent statiques. Aucun processus n'est lancé
  pendant la configuration, la compilation ou l'installation.

Le worker doit correspondre à l'ABI et au protocole de la bibliothèque (version 4).
Les anciens workers sont refusés. Cette option n'active ni tests ni mesures et ne
recherche aucun plugin installé.

## Deux modes audio

`PluginScheduling::pipelined` est le mode par défaut. `process()` envoie le bloc
courant et récupère le précédent, sans attente. La taille est fixe et égale au
`maximumFrames` de l'activation. La latence du pipeline vaut un bloc et se cumule
aux frontières de workers en série. `PluginInstance::timing()` l'inclut dans la
latence totale quand la latence native est connue. Une latence native inconnue
reste inconnue. Le `PluginWorker` bas niveau expose séparément ces deux valeurs.

`PluginScheduling::sameCycle` supprime le retard fixe du pipeline. L'application
utilise les trois opérations suivantes sur un propriétaire audio sérialisé :

1. `submit(block, ticket)` copie les entrées et événements dans la mémoire préparée.
   Une seule requête peut être en vol. Un second envoi renvoie `transition_in_progress`.
2. Le moteur exécute d'autres nœuds prêts de son graphe.
3. `collect(block, ticket)` récupère le résultat de CE bloc. `would_block` signifie
   que le résultat manque et remplit les sorties de silence ; on peut réessayer tant
   qu'il reste du travail utile et du temps dans le cycle.
4. À l'échéance, `discard(ticket)` abandonne la requête. Une réponse tardive ne peut
   jamais être utilisée comme le résultat du bloc suivant. Si le worker traite encore
   la requête abandonnée, l'envoi suivant renvoie `would_block` jusqu'à sa fin.

Les appels ne bouclent pas en attendant le worker, ne dorment pas, n'interrogent pas
son processus et n'allouent/libèrent pas de mémoire. Un signal non bloquant peut
réveiller son thread audio. `process()` n'est pas l'API de ce mode et renvoie
`invalid_argument`. Les tailles entre les bornes préparées peuvent varier.

Le mode même cycle ne garantit pas qu'un plugin termine à temps. Il expose les
primitives nécessaires à un ordonnanceur ; CyAudioCore n'invente pas le graphe de
l'application. Il faut un travail indépendant suffisant ou un ordonnanceur de rendu
qui respecte l'échéance. Ne pas ajouter une boucle d'attente autour de `collect()`
dans le callback périphérique. Les boucles avec attente dans les tests/outils sont
uniquement destinées à la validation hors temps réel.

## Contrôles, CLAP et éditeurs

Les méthodes de contrôle RPC s'exécutent sur le propriétaire UI, avec délai borné.
Activation, désactivation, changements de disposition et sauvegarde/restauration
exigent le propriétaire audio au repos. `start/stop/reset` publient des commandes
non bloquantes, exécutées sur le thread audio du worker. Une erreur de démarrage
étranger est visible dans le résultat audio ; elle ne lance aucun traitement local.
Les commandes d'arrêt/démarrage peuvent se regrouper avant leur consommation ;
le dernier état demandé est conservé. Une demande de reset reste distincte.

`pumpMainThread()` reste nécessaire pour surveiller le processus, publier les
changements de latence, traiter les redimensionnements et livrer les notifications.
Les requêtes de redémarrage/changement de ports restent explicites : arrêter,
désactiver, rafraîchir les descriptions et reconstruire le routage avant réactivation.
Les réponses de contrôle et audio occupent des zones différentes ; un pump normal
ne consomme ni n'abandonne le ticket audio en vol.

Les contrôles publics stables de `ClapExtensions` sont transportés par le worker :
configurations/ports, dispositions spatiales, voix, noms de notes, pages de contrôle,
contexte de piste, conversions de paramètres, indications, états contextualisés,
chargement de presets, menus et préférences GUI. Aucun pointeur CLAP ni pointeur
applicatif n'est transmis. Le statut de traitement est publié par une valeur atomique.

`flushAudio()` et l'activation de ports en état actif utilisent un échange audio non
bloquant : le premier appel renvoie `would_block`, puis le même appel est réessayé
jusqu'à réception du résultat. Ne pas lancer d'autre échange audio pendant cette
opération ; conserver les arguments jusqu'à sa fin. Les capacités préparées bornent
les événements. Les appels de contrôle inactifs gardent leur résultat synchrone.

Les logs, demandes de rescan/clear, suggestions et notifications de preset sont
copiés dans une file bornée et livrés au prochain pump, sur l'UI hôte. Les menus
nécessitant une réponse utilisent un canal inverse borné ; l'hôte le sert pendant
ses RPC et ses pumps. Sans pump, la demande expire dans le worker. Les callbacks
applicatifs restent du code de confiance, sans réentrance/destruction de l'instance.

Un `ClapHostServices::executeTasks` applicatif ne traverse pas le processus et est
refusé explicitement. `isolation.clapThreadPool=true` propose un exécuteur **sériel**
local au worker. Il accomplit toutes les tâches demandées, sans pool, attente ou
pointeur de fonction transmis par IPC ; il ne promet aucun parallélisme DSP.

L'éditeur reste dans le processus du plugin. L'intégration native Win32 est validée ;
l'intégration d'éditeurs Cocoa/X11/Wayland attend ses validations de plateforme.
Les restrictions des extensions draft restent celles du contrat CLAP existant.

## Incidents et limites

Un crash ordinaire ferme le processus du plugin. Le moteur peut continuer avec une
sortie silencieuse ; son propriétaire de contrôle reçoit `disconnected`. Un blocage
est détecté par le heartbeat ou l'échéance RPC ; le processus possédé est terminé
hors audio. L'application recrée l'instance, recharge un état sauvegardé et reprend
le routage. Il n'y a pas de relance automatique en boucle ni de bypass implicite.
Un effet sans réponse et un instrument sans réponse produisent tous deux du silence.

L'isolation contient les défaillances de plugins ; elle ne garantit pas la survie
contre une défaillance du noyau, d'un pilote ou du code du moteur. Les processus
conservent les permissions utilisateur : pas de confinement fichiers/réseau.
Le code DSP étranger conserve son propre comportement temps réel.

La validation automatique utilise exclusivement nos modules de test, notamment des
sorties abruptes et des blocages contrôlés. Aucun périphérique physique n'est ouvert.
Voir [validation CLAP](ClapValidation.md) pour les campagnes exécutées et leurs limites.

Le contrat de threads suit les [règles CLAP 1.2.10](https://github.com/free-audio/clap/blob/1.2.10/include/clap/ext/thread-check.h).
Voir aussi [les résultats de validation de l’isolation](PluginIsolationValidation.md).

## Veille des plugins

La politique automatique, les réveils et les compteurs sont décrits dans le
[manuel des workers](https://mrmybal.github.io/CyAudioCore/#/fr/workers/sleep).
Les plugins restent isolés et chargés pendant la veille. Continuez d'appeler
l'API audio habituelle et de pomper le contrôle : Core évite les calculs et les
échanges audio inutiles sans supprimer l'éditeur ni la surveillance des plantages.
Le protocole 4 exige de reconstruire ensemble bibliothèque et worker.
