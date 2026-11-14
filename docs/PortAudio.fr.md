# Fournisseurs d'E/S audio

`AudioProvider` choisit **miniaudio** (défaut) ou **PortAudio**.
`AudioBackend` choisit l'API système : WASAPI, WinMM, CoreAudio, ALSA, JACK, etc.
Les deux fournisseurs implémentent les mêmes `AudioDeviceContext`, `AudioDevice`
et `AudioDeviceManager`. Aucun type de leurs SDK ne figure dans les headers publics.

## Compilation

```sh
cmake -S . -B build/pa -DCYAUDIOCORE_CYDSP_SOURCE_DIR=../CyDSP -DCYAUDIOCORE_WITH_PORTAUDIO=ON
cmake --build build/pa --config Release
cmake --install build/pa --config Release --prefix build/install-pa
```

`CYAUDIOCORE_WITH_PORTAUDIO` est **OFF** par défaut et exige
`CYAUDIOCORE_BUILD_DEVICES=ON`. Les sources épinglées sont dans le dépôt : la
configuration ne télécharge rien. Le fournisseur et le runtime sont statiques ;
`CyAudioCore::Devices` conserve sa dépendance PortAudio privée. L'installation
exporte son archive auxiliaire pour le lien statique d'un consommateur, sans
installer les headers PortAudio dans l'API de CyAudioCore.

| Plateforme | Option | Défaut | Dépendance système |
| --- | --- | --- | --- |
| Windows | `CYAUDIOCORE_PORTAUDIO_WASAPI` | ON | SDK Windows |
| Windows | `CYAUDIOCORE_PORTAUDIO_WINMM` | ON | SDK Windows |
| Windows | `CYAUDIOCORE_PORTAUDIO_DIRECTSOUND` | OFF | SDK Windows |
| macOS | `CYAUDIOCORE_PORTAUDIO_COREAUDIO` | ON | SDK Apple |
| Unix desktop | `CYAUDIOCORE_PORTAUDIO_ALSA` | ON | Développement libasound |
| Unix desktop | `CYAUDIOCORE_PORTAUDIO_JACK` | OFF | Développement JACK, pkg-config |

Ces options concernent uniquement PortAudio et sont exposées sur leur plateforme.
Une dépendance activée et absente fait échouer la configuration. Aucun SDK
propriétaire n'est découvert automatiquement. ASIO, WDM-KS et OSS ne sont pas
activés par cette recette ; un éventuel ajout ASIO demanderait un choix explicite
de SDK et de licence. Android et WebAudio ne sont pas couverts par ce fournisseur.

PortAudio v19.7.0 est épinglé au commit
`147dd722548358763a8b649b3e4b41dfffbcfbb6`. La licence permissive de type MIT et
les attributions sont conservées dans la [notice](../third_party/licenses/portaudio.txt)
et le [manifeste](../third_party/manifest.json). Une recette CMake originale compile
les sources amont inchangées sans utiliser la découverte de SDK du CMake amont.
Les headers d'extensions non compilées ne constituent pas des backends activés.
Les anciens headers de compatibilité SDK MinGW du dépôt amont sont exclus de
l'import ; fournir les headers du SDK de la plateforme et du compilateur choisis.

## Exemple minimal

Cet extrait s'insère dans une fonction retournant `cyaudio::Status`. Un
[exemple compilé](../examples/AudioProviders.cpp) accepte le callback du moteur.

```cpp
#include <cyaudio/io/AudioDeviceManager.h>

// Objet détenu par l'application ; destruction après arrêt des callbacks.
cyaudio::AudioDeviceManager audio;
cyaudio::AudioContextOptions options;
options.provider = cyaudio::AudioProvider::portAudio;
options.backend = cyaudio::AudioBackend::wasapi; // coreAudio / alsa / jack selon plateforme
auto status = audio.prepare(options);
if (status != cyaudio::Status::ok) return status;

cyaudio::AudioDeviceConfig config;
config.sampleRate = 48000;
config.outputChannels = 2;
config.maxCallbackFrames = 512;
status = audio.openWithContext(config,
    [](void*, cyaudio::AudioBlock<const float>, cyaudio::AudioBlock<float> output,
       const cyaudio::AudioCallbackContext&) noexcept {
        // Produire output depuis le moteur préparé. La sortie est déjà silencieuse.
        (void)output;
        return cyaudio::Status::ok;
    }, nullptr);
// Vérifier status. Sur le contrôle : audio.poll(nowMs), puis audio.close().
```

`open` du gestionnaire démarre le flux. Avec `AudioDevice`, appeler `open`, puis
`start`, `stop`, `close` explicitement. Les anciens appels `prepare()`, `prepare(true)`
et `AudioContextOptions{AudioBackend::wasapi}` continuent de choisir miniaudio.
Les champs sont ajoutés en fin des agrégats existants ; recompiler les consommateurs
avec les headers et archives correspondants, sans supposer de compatibilité ABI binaire.
`context.provider()`, les descriptions et les entrées d'énumération exposent le
fournisseur. Une sélection indisponible retourne `unsupported_backend` ; un
périphérique absent retourne `device_unavailable`. Il n'existe aucun repli entre
fournisseurs ou backends. Le backend automatique PortAudio est choisi à la
préparation, puis reste fixé pendant la vie du contexte.

## Buffers et temps réel

Le contrat applicatif reste float32 planaire. PortAudio demande d'abord
`paFloat32 | paNonInterleaved` : des tables préparées construisent les vues
`AudioBlock` directement sur ses échantillons. Un refus de disposition avec
`paSampleFormatNotSupported` permet de réessayer le même backend en float32
entrelacé, avec les conversions publiques CyDSP dans des buffers préparés.
Les autres erreurs ne déclenchent pas de repli.

`AudioAccess::exclusive` est pris en charge par ce fournisseur uniquement avec
WASAPI compilé ; les autres demandes exclusives échouent sans ouvrir un flux partagé.

Les canaux `activeInputChannels` / `activeOutputChannels` sont des indices du flux
client natif, dans l'ordre logique applicatif. Le flux demande jusqu'au plus grand
indice inclus. Les sorties actives dupliquées sont interdites. Les sorties non
sélectionnées sont nulles ; les entrées absentes, même une plane individuelle,
sont remplacées par des zéros. Une erreur applicative rend silencieux le sous-bloc
concerné. Chaque sortie native est entièrement remplie.

`inputChannelMap` / `outputChannelMap` sont les positions de haut-parleurs du contrat
miniaudio existant. Le PortAudio portable ne les implémente pas : une demande non
vide échoue explicitement. Elles ne sont pas interprétées comme des indices actifs.

Le callback PortAudio utilise `paFramesPerBufferUnspecified`. `periodFrames` donne
une suggestion de latence, pas une période matérielle garantie. Les appels dépassant
`maxCallbackFrames` sont découpés avec des offsets et horloges cohérents. Notre
chemin préparé n'alloue/libère pas, ne verrouille pas, n'attend pas et ne fait pas
d'E/S. Aucun appel d'API PortAudio n'est effectué depuis nos callbacks audio/de fin.
Préparation, API de contrôle, reconfiguration et destruction appartiennent au
contrôle sérialisé. Les callbacks doivent être arrêtés avant de changer leur contexte.

## Horloges et reconnexion

`PaTime` a une origine non spécifiée. Chaque callback associe son `currentTime`
à l'instant d'entrée `steady_clock` et transpose seulement les différences ADC/DAC.
Ces estimations portent `backendClockEstimate` ; elles ne mesurent ni dérive entre
horloges ni retard de livraison du callback. Valeurs absentes, nulles, non finies ou
hors domaine restent inconnues. `clock`/`callbackTime` utilisent directement le
domaine steady_clock. Le gestionnaire transmet le contexte natif ; les anciens
drivers personnalisés gardent leur pont historique.

Les latences de `Pa_GetStreamInfo` portent `backendDeclared`. Les nombres de canaux,
taux et périodes matériels restent inconnus : maxima de canaux et taux par défaut
de `PaDeviceInfo` ne prouvent pas une configuration matérielle. Le taux de flux est
exposé par l'API entière existante, arrondi à l'hertz. Formats natifs, noms de canaux,
plages de buffers et garanties duplex ne sont pas inventés. `deviceXruns` compte
les callbacks portant au moins un flag d'underflow/overflow PortAudio ; les coûts
de callback et dépassements de délai sont séparés.

Le callback de fin publie une notification atomique. `running` vérifie l'activité
sur le contrôle. `AudioDeviceManager::poll` détecte l'arrêt et ferme/rouvre selon
les délais existants ; chaque nouveau flux reçoit une nouvelle génération.

Le catalogue portable est un instantané d'initialisation. Les contextes partagent
un runtime avec comptage de références et de flux. Une énumération le rafraîchit
par terminate/initialize uniquement lorsqu'**aucun flux PortAudio n'est ouvert**.
Un ajout pendant qu'un autre flux reste ouvert peut donc ne pas être visible avant
sa fermeture. Le signalement des déconnexions dépend aussi du backend système.

Les identités miniaudio restent inchangées. Les tokens PortAudio
`cyaudio.portaudio/1/...` sont propres au contexte et proviennent du backend, de la
direction et du nom, sans index d'énumération persistant. `persistentIdentity=false`
et `persistentId` vide sont explicites. Les réordonnancements sont tolérés dans ce
contexte pour les noms qui restent uniques. Dès qu'un nom est ambigu pour une même
direction/backend, tous ses tokens portent un suffixe `/catalogue/<génération>/<ordinal>`.
Ils permettent de choisir chaque entrée de cet instantané, sans attribuer à
l'ordinal une identité matérielle. Les lectures répétées du même instantané
conservent ces tokens ; un changement de sa structure ou toute réinitialisation
PortAudio les invalide, même si les noms et les indices semblent identiques.
Cette règle reste appliquée au nom jusqu'à la destruction du contexte, même si
un seul périphérique subsiste : il ne récupère aucun ancien token. Un
rafraîchissement effectué par un autre contexte partageant le runtime est aussi pris
en compte. `open` et `capabilities` refusent un ancien token avec
`device_unavailable`. Réénumérer puis sélectionner explicitement un nouveau token
permet de rouvrir le périphérique. Le gestionnaire ne reconnecte donc pas une
ancienne sélection ambiguë sur une autre interface ; un repli par défaut reste
possible uniquement si l'application l'a demandé.

Tant qu'aucun flux n'est ouvert,
le catalogue n'est réinitialisé qu'une fois par intervalle (2 s par défaut), ce qui
peut retarder d'autant la détection d'un rebranchement. Après recréation du contexte,
réénumérer et employer un sélecteur avec
`provider=portAudio` ; tout repli par nom doit être explicitement accepté.

## Validation et références

Les tests injectent une API PortAudio simulée, sans appeler son initialisation
réelle. Les tests miniaudio emploient le backend null. Les vérifications exécutées
et limites sont dans le [rapport](PortAudioValidation.md).

`scripts/run_native.py --portaudio --services-only` exécute la suite des services
et compile leurs headers publics. Ajouter `--sanitize` pour AddressSanitizer ;
omettre `--portaudio` vérifie la configuration par défaut. Sans `--services-only`,
le script conserve aussi les tests indépendants de CyDSP.

Références officielles : [contrat des callbacks et API](https://portaudio.com/docs/v19-doxydocs/portaudio_8h.html),
[horodatages](https://portaudio.com/docs/v19-doxydocs/structPaStreamCallbackTimeInfo.html),
[sources et licence épinglées](https://github.com/PortAudio/portaudio/tree/147dd722548358763a8b649b3e4b41dfffbcfbb6).
