# Transport : éviter l'effacement avant lecture — 14 novembre 2026

`AudioTransport::process` effaçait chaque sortie entière avant de demander à la
source d'y écrire. Le chemin de lecture complet effectue maintenant uniquement
l'écriture de la source. Les zones non remplies restent explicitement mises à
zéro : pause, fin de source, lecture partielle, erreur et trous d'underrun avec
avance de la position. Les sous-blocs et les canaux conservent leurs limites.

Ce changement utilise le contrat de destination de `AudioSource::readAt` : les
échantillons annoncés comme lus doivent être écrits par la source ; celle-ci ne
doit pas compter sur une destination préinitialisée. Ce contrat est précisé dans
le header. Une source qui ajoute au contenu précédent doit produire son résultat
complet pour implémenter cette interface de lecture.

Les API, commandes, positions, boucles et politiques d'underrun restent les
mêmes. Aucune préparation, allocation, libération, attente ou synchronisation
supplémentaire n'entre dans le callback. Le backend VectorOps reste préparé sur
contrôle. Aucun traitement numérique CyDSP n'est réimplémenté.

## Mesure du correctif

La comparaison utilise le banc existant `tests/PathMeasurements.cpp`, inchangé,
avec un exécutable avant conservé à la base Core `2e954fa` et un exécutable après
recompilation. Le même DSP figé `fda6bd30c5b622795cc08e8bd2750188a6c5a0c0` est lié
des deux côtés pour isoler le changement Core. Windows x64, MSVC 19.40 Release,
`/O2 /fp:strict`. Aucun périphérique n'est ouvert.

Chaque cas porte sur le groupe entier de **32 transports mémoire stéréo**,
préparés puis arrêtés ou en lecture bouclée. Les sorties des 32 transports sont
distinctes ; la source synthétique est partagée et les caches sont chauds.
Cinq paires de processus neufs alternent l'ordre avant/après. Chaque cas chauffe
64 appels, puis mesure 31 moyennes de lots de 32 appels. Les valeurs présentées
sont les médianes des cinq médianes ; ce ne sont pas des P95 de callback.

| Groupe de 32 transports | Frames | Avant µs/bloc | Après µs/bloc | Variation de durée |
| --- | ---: | ---: | ---: | ---: |
| Lecture bouclée | 128 | 4,147 | 3,431 | −17,3 % |
| Lecture bouclée | 1 024 | 6,909 | 4,856 | −29,7 % |
| Arrêtés, contrôle | 128 | 1,713 | 1,653 | −3,5 % |
| Arrêtés, contrôle | 1 024 | 3,066 | 3,078 | +0,4 % |

Le cas arrêté reste proche du témoin ; aucun gain n'est revendiqué pour lui.
À 48 kHz/1 024 frames, les 2,053 µs économisées par groupe représentent environ
**0,0096 point de CPU d'un cœur normalisé**, avec un appel par bloc. Ce n'est pas
une mesure du CPU CyDAW : le correctif seul ne peut expliquer ou supprimer tous
les écarts du rendu applicatif.

Les [données individuelles et empreintes](measurements/transport-output.json)
sont publiées séparément des journaux locaux sous `build/transport-output`.
Le pilote refuse les
frontières avec compilation, changements de processus CyDAW ou activité
applicative supérieure à 1 % d'un cœur ; il refuse aussi une activité système
moyenne supérieure à deux cœurs logiques, banc compris. Ces observations ne
garantissent pas l'absence de toute préemption pendant un appel.
Trois paires perturbées sont conservées localement et exclues. Une compilation
extérieure a interrompu le pilote avant la dernière paire ; les quatre paires
valides déjà recueillies ont été reprises depuis leurs journaux, puis une
cinquième paire a été ajoutée avec les mêmes exécutables et critères.

## Correction et limites dans le diagnostic CyDAW

Cette optimisation enlève un travail Core réel. Elle ne mesure pas le gain de
l'application complète et ne corrige pas à elle seule les écarts d'analyse ou de
graphe du rapport CyDAW. Aucun de ces écarts n'est déclaré résolu ici.

La lecture du code applicatif précise les prochaines adaptations :

- `publishAnalysisFrame` utilise sa propre accumulation double pour le RMS et
  les pics, puis une boucle de mix mono pour la télémétrie. Il n'appelle pas
  `SampleBuffer::getRMSLevel`. Le microbenchmark de ce dernier ne représente
  donc pas directement le surcoût de cette fonction. Une variante à accès par
  plans préparés doit conserver l'ordre des accumulations, les valeurs RMS/pics,
  le contenu de l'anneau et la publication des trames.
- Le transport adapté efface la sortie, utilise un scratch, applique une rampe
  même à gain constant et recopie le résultat. L'API Core accepte déjà un
  `AudioBlock` emprunté : un chemin direct applicatif est envisageable quand les
  canaux et la sous-plage correspondent. Les conversions et cas de canaux
  différents nécessitent toujours un chemin approprié.
- Préparer les VectorOps de `SampleBuffer` relève du contrôle ; les vues créées
  sur audio doivent recevoir un dispatch déjà préparé. Modifier son constructeur
  pour découvrir le CPU à chaque vue réintroduirait ce travail sur audio.
- Le worker mesuré dans CyDAW reste son `IsolatedPluginHost`, avec son propre
  protocole. Un changement de `cyaudio::PluginWorker` ne peut pas être présenté
  comme sa correction sans passer par les appels réellement utilisés.

CyDAW et CyDSP sont consultés en lecture seule. Ces adaptations applicatives
restent à intégrer et à mesurer dans la campagne CyDAW ; aucune amélioration de
leur rendu complet n'est déduite du seul coût du transport Core.

## Vérification

Les tests de PCM couvrent float/double, backends scalaire/automatique, deux
canaux, sous-plages avec gardes, tailles variables et nulles, pause/reprise,
fin anticipée, lectures partielles, erreurs avec et sans préfixe valide,
compteurs invalides et trous d'underrun traversant plusieurs tours de boucle.
Chaque appel de ces scénarios passe par l'audit d'allocations/libérations audio.
Le consommateur CMake installé vérifie aussi lecture, pause et fin de source du
transport en liant les archives distribuées.

Régressions complètes Core et compilation indépendante des headers publics :
réussies en Release avec le DSP historique/PortAudio OFF, puis avec le DSP actuel
et PortAudio ON, et sous AddressSanitizer avec ce dernier. Les consommateurs
CMake installés et déplacés passent avec PortAudio ON et OFF. Les périphériques
sont les fixtures simulées/nulles des tests ; aucun matériel physique n'est ouvert.

Les mesures utilisent le DSP historique identique des deux côtés. La
compatibilité est vérifiée séparément avec le DSP actuel figé
`ea56508ad3e26d790e491140b7f18a95e4411d5d`, incluant ses optimisations récentes.
Les tests matériels ASIO, la latence physique, les xruns et les autres plateformes
ne font pas partie de cette validation.

Pour reproduire les mesures, conserver `cyaudio_path_measurements.exe` à la base
`2e954fa`, reconstruire cette même cible après le correctif avec le même DSP et
les mêmes options, puis exécuter les deux binaires sans argument dans cinq paires
de processus neufs en alternant l'ordre. Comparer les lignes
`transport32_playing`/`transport32_paused` à dimensions identiques. La cible se
construit explicitement avec `CYAUDIOCORE_BUILD_PATH_MEASUREMENTS=ON` ; aucune
campagne n'est exécutée par défaut. Les sources du banc restent inchangées.
