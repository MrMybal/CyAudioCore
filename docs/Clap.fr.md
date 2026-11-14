# CLAP, format de plugins principal

CyAudioCore privilégie CLAP pour l'hébergement des effets et instruments. L'appel
`createPluginProvider(provider)` sélectionne CLAP ; la surcharge avec `PluginFormat`
conserve le choix explicite des autres formats. Aucun changement de format silencieux.

Le contrat de référence est [CLAP 1.2.10](https://github.com/free-audio/clap), révision
`a47f6badb49d948fd009998f28309cdab78979c9`, licence MIT. Les interfaces restent privées
à `CyAudioCore::Plugins`, archive statique. Les notices amont sont inchangées.
Cette intégration est un hôte ; elle ne fournit pas d'export de plugin CLAP.
La licence MIT de l'interface ne remplace pas la licence propre de chaque plugin chargé.

## Contrat public

Inclure `cyaudio/plugins/ClapExtensions.h` et lier `CyAudioCore::Plugins`.
Le SDK CLAP n'est requis ni dans les includes ni dans les dépendances du consommateur.
`PluginInstance::clapExtensions()` renvoie une interface empruntée, valable jusqu'à
la fermeture ou au remplacement de l'instance. Elle est nulle pour les autres formats.
Les archives et consommateurs C++ doivent être reconstruits ensemble après cette
extension de l'API 0.1 en développement.

```cpp
std::unique_ptr<cyaudio::PluginProvider> provider;
auto status = cyaudio::createPluginProvider(provider);
if (status != cyaudio::Status::ok) return status;

cyaudio::ClapHostServices services;
services.track = cyaudio::ClapTrackInfo{};
services.track->name = "Synthétiseur";
services.track->channels = 2;
services.track->portType = "stereo";

cyaudio::PluginHostCallbacks callbacks;
callbacks.clap = &services;
cyaudio::PluginInstance plugin;
status = provider->create(moduleAbsolu, identifiantClap, callbacks, plugin);
if (status != cyaudio::Status::ok) return status;

auto* clap = plugin.clapExtensions();
if (clap->capabilities().audioConfigurations) {
    std::vector<cyaudio::ClapAudioConfiguration> configurations;
    status = clap->audioConfigurations(configurations);
    if (status != cyaudio::Status::ok) return status;
    // L'application choisit explicitement un identifiant de configuration.
}
cyaudio::PluginPrepareOptions spec;
spec.sampleRate = 48000;
spec.maximumFrames = 256;
return plugin.activate(spec);
```

[ClapHost.cpp](../examples/ClapHost.cpp) est compilé par les tests opt-in. L'application
construit ses bus depuis `description()`, prépare les événements, puis appelle
`start/process/stop` sur son propriétaire audio sérialisé. Float32 et float64 restent
négociés avec le plugin ; aucune conversion en double n'est prétendue quand il ne
le supporte pas. Les blocs peuvent varier entre les bornes préparées.

## Extensions stables

Les capacités annoncées par `capabilities()` reflètent les fonctions réellement
fournies par le plugin. Leur absence donne `unsupported_format`. Les identifiants
de compatibilité explicitement reconnus par CLAP 1.2.10 sont négociés également.
Les versions majeures étrangères à l'ABI 1 sont refusées.

| Interface CLAP stable | Intégration CyAudioCore |
| --- | --- |
| `audio-ports`, `note-ports` | Bus distincts, sidechains, dialectes, rescan et identifiants stables |
| `audio-ports-config`, `audio-ports-config-info` | Liste, configuration courante, détails des bus et sélection inactive |
| `configurable-audio-ports` | Vérification et application groupée de dispositions mono/stéréo/surround/ambisoniques |
| `audio-ports-activation` | Activation inactive ou sur le propriétaire audio si le plugin l'autorise ; buffers nuls et masques constants pour les ports désactivés |
| `surround`, `ambisonic` | Cartes de canaux, ordre/normalisation, interrogation des dispositions supportées |
| `params` | Valeurs natives, texte dans les deux sens, tous les indicateurs, automation/modulation par note/port/canal/touche, gestes et flush |
| `param-indication` | Indications de mapping et d'automation, couleur et libellés |
| `state`, `state-context` | Flux bornés et contextes preset/duplication/projet ; repli standard prévu par CLAP si l'extension de contexte manque |
| `preset-load` | Emplacement fichier absolu ou preset interne, clé, notifications de chargement et erreurs natives |
| `remote-controls` | Pages de huit contrôles, métadonnées, changements et suggestions |
| `note-name`, `voice-info` | Noms de notes et informations de polyphonie, y compris notes superposées |
| `track-info` | Nom, couleur, canaux et rôle de piste, disponibles dès `init` si renseignés |
| `render` | Temps réel/hors ligne ; refus du hors ligne si le plugin exige le temps réel |
| `latency`, `tail` | Valeurs connues/inconnues et queue infinie explicites, notifications différées |
| `gui` | API préférée, interface native, fenêtre flottante, titre, taille, contraintes, redimensionnement, DPI et visibilité |
| `context-menu` | Menus du plugin et actions ; menus/popups de l'application par callbacks sans toolkit imposé |
| `thread-check` | Propriétaires main/audio, y compris les tâches du pont d'exécution parallèle |
| `thread-pool` | Exécuteur optionnel fourni par l'application ; aucune annonce en son absence |
| `log` | File bornée de diagnostics ; livraison uniquement dans `pumpMainThread`, débordement signalé |
| `timer-support`, `posix-fd-support` | Pompage main, 64 timers/64 descripteurs ; descripteurs POSIX annoncés uniquement hors Windows |
| `event-registry` | Réponse négative explicite aux espaces personnalisés inconnus ; seuls les événements core sont traduits |

La factory stable `preset-discovery` est exposée par `ClapPresetDiscovery` : liste
des fournisseurs, déclarations de types/emplacements/soundpacks et lecture des
métadonnées. Le module reste initialisé jusqu'à la fermeture du dernier utilisateur.
Aucun disque n'est parcouru automatiquement. Le catalogue ne charge pas les presets
à la place de l'application. Les chaînes ont une limite de 65 535 octets, le budget
texte est de 64 MiB, le nombre de presets est borné à 65 536 et les listes annexes
à 4 096 éléments. Un échec ne remplace pas les résultats précédemment reçus.

Les en-têtes `draft/` et extensions tierces sont distincts des interfaces stables.
Ils ne sont pas annoncés automatiquement. L'inclusion de leurs déclarations dans
l'archive amont ne constitue pas une implémentation. Cela concerne notamment les
propositions de tuning, undo, scratch memory, project location, triggers ou webview.

## Propriétaires et temps réel

Les méthodes avancées sont main-thread, sauf la lecture de `lastProcessStatus()`
et `setAudioPortActive()` sur une instance active, réservé au propriétaire audio.
Les changements de disposition, de preset et les opérations d'état avec contexte
exigent une instance inactive. Une relecture de ports échouée interdit l'activation
jusqu'à une relecture réussie ; l'ancienne description ne sert pas à traiter le
nouveau routage. Un simple rafraîchissement de paramètres conserve les activations
de ports ; une sélection de configuration ou un rescan de liste les invalide.

`pumpMainThread` remonte les demandes précises. L'application doit traiter restart,
ports, paramètres, noms de notes, pages de contrôle, voix et presets. Les callbacks
`parameterRescan` et `parameterClear` conservent leurs drapeaux et identifiants.
Un rescan complet invalide immédiatement les cookies opaques ; les événements
utilisent ensuite un cookie nul valide jusqu'à la prochaine relecture.

`pluginParameterFlush` demande un échange de paramètres. Avec audio actif,
`process()` réalise déjà cet échange. En l'absence de bloc audio, le propriétaire
audio peut appeler `flushAudio()`. Inactif, utiliser `flush()` sur main. Ces appels
ne se chevauchent jamais et n'acceptent que valeurs, modulations et gestes de paramètres.
Les valeurs CLAP peuvent être interrogées sur main pendant le traitement, conformément
au contrat natif. Les métadonnées de paramètres décrivent leur valeur physique native.

Les notes CLAP, expressions, modulations polyphoniques, MIDI/MPE, SysEx et UMP sont
des événements préparés ; les dialectes du port sont respectés. `noteEnd` est une
notification de sortie. Les événements étrangers ou trop grands sont rejetés et
signalés sans annuler l'audio valide. Les informations de transport comprennent
les positions, la signature et la variation de tempo par échantillon.

Les retours `continue`, `continueIfNotQuiet`, `tail` et `sleep` restent accessibles
à l'ordonnanceur par `lastProcessStatus()`. CyAudioCore ne supprime pas seul les
appels futurs : réveil, silence d'entrée, événements et fin de queue appartiennent
à l'ordonnanceur de l'application. Une erreur de traitement remplit les sorties de zéros.

L'adaptateur préparé n'alloue ni ne libère, ne verrouille, n'attend et n'accède pas aux
fichiers. La callback native de log copie au plus 511 octets dans 64 emplacements,
sans appel du logger applicatif sur audio. Les messages excédentaires sont perdus
et signalés par `pluginLogsDropped`. L'application ne doit pas détruire/réentrer
l'instance depuis un callback de contrôle.

Le pont `executeTasks` exige que l'exécuteur termine toutes ses tâches avant de
retourner vrai, ou n'en exécute aucune et retourne faux. Il rejette les demandes
hors `process`, récursives ou de plus de 65 536 tâches. CyAudioCore n'introduit pas
un pool bloquant dans le chemin audio. Un exécuteur applicatif ou un plugin étranger
garde la responsabilité de son propre comportement temps réel.

## Worker et plateformes

Le worker existant héberge toujours CLAP via le même adaptateur et conserve son
protocole version 2 et son bloc de latence. Audio, événements core, états standards,
GUI et demandes continuent de traverser ce protocole. L'interface avancée empruntée
et les callbacks applicatifs locaux ne traversent pas le processus : les contrôles
spécifiques de `ClapExtensions` sont actuellement utilisables en processus.
Les changements de géométrie du worker exigent toujours une nouvelle instance de
mailbox. La découverte en processus exécute du code étranger et n'est pas un sandbox.

Win32 natif est exécuté dans les tests. Les chemins Cocoa/X11 et le pompage POSIX
exigent encore une acceptation sur leurs OS. Wayland intégré est refusé conformément
au contrat stable ; seule une fenêtre flottante est autorisée. UIKit n'est pas
exposé par l'API de fenêtres de CyAudioCore. Une compatibilité universelle avec tous
les plugins et toutes les extensions tierces ne se déduit pas de cette matrice.

Les fenêtres flottantes peuvent négocier une API nulle avec
`PluginWindowApi::unspecified`, sans parent. Dans ce cas, aucune unité de DPI n'est
supposée par l'hôte. Le titre peut être fourni via `suggestEditorTitle()`.

## Validation explicite

`CYAUDIOCORE_BUILD_TESTS=ON` ajoute la suite déterministe `clapExtensions`, sans
périphérique ni plugin installé. Elle couvre ports/configurations/cartes, polyphonie,
modulation et transport, blocs variables, float/double, ports absents, cookies,
flush actif, diagnostics bornés, pont de tâches, états, menus, presets et nettoyage.
Les tests CLAP/worker et des autres formats restent inclus.

`CYAUDIOCORE_BUILD_CLAP_ACCEPTANCE=ON`, désactivé par défaut, construit séparément
`cyaudio_clap_acceptance`. Ce programme ne tourne pas pendant CMake, la compilation
ou CTest ; son lancement explicite prend un module absolu et un identifiant :

```text
cyaudio_clap_acceptance <module-absolu.clap> com.github.free-audio.clap.gain
```

Il peut exécuter les plugins de référence officiels compilés séparément en mode
headless : blocs variables, notes, audio fini, gain unité de référence et état.
Ce mode charge du code étranger ; il n'ouvre aucune E/S audio. Aucune dépendance
supplémentaire n'est liée à la bibliothèque de production et aucun téléchargement
n'est implicite. Voir les [résultats et limites de validation](ClapValidation.md).
