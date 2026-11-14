# Profilage des chemins CyAudioCore — 13 novembre 2026

Le [correctif ultérieur du transport](TransportOutputOptimization.fr.md) retire
un effacement redondant dans Core. Ce rapport conserve les mesures de diagnostic
antérieures et leurs limites.

Le buffer de compatibilité utilisé par CyDAW présente une piste mesurée : ses
`VectorOps` restent scalaires. Préparer un backend automatique accélère fortement
son RMS. Dans cette charge isolée, le gain ne représente toutefois qu'environ
0,15 point de CPU d'un cœur pour 32 pistes. Il ne suffit pas à expliquer les
13,43 % du processus parent relevés dans le diagnostic CyDAW avec huit plugins
au repos. Les coûts isolés ci-dessous ne permettent pas d'attribuer le reste à
une bibliothèque sans les piles CPU et les fréquences d'appel de l'application.

Cette livraison ajoute des bancs de mesure et leur pilote explicite. Elle ne
modifie aucun traitement de production ni aucune API publique.

## Références et périmètre

La comparaison d'application fournie dans
`CyDAW_Project/docs/CyAudioCoreComparison.fr.md` compare deux états complets du
produit. Son commit `de66021` désigne la migration **CyDAW** ; la bibliothèque
Core effectivement utilisée est `bb26e93604d43de2069694b0791bc0bcf5a53ea6`, avec
CyDSP `fda6bd30c5b622795cc08e8bd2750188a6c5a0c0`.

Les nouveaux bancs sont ajoutés à cette base Core. Ils utilisent une archive Git
locale figée de ce même commit DSP : les modifications DSP concurrentes ne sont
pas incluses. Le probe facultatif compile seulement trois headers CyDAW en
lecture seule, à la révision `1a75df5198e1ea6fea8fa0f29906004d088e93e9`.
Leurs empreintes et celles des entrées du compilateur sont conservées dans les
[données de mesure](measurements/core-paths-fda6bd3.json).

Windows x64, MSVC 19.40 Release, Ryzen 9 7900X ; `/O2`, FP strict côté
Core/DSP, `/fp:precise` pour le probe du header applicatif. Le dispatch automatique
observé est **AVX-512**. Aucun flag SIMD global n'est ajouté. Les buffers sont
stéréo float32, à 128 et 1 024 frames ; les tableaux suivants présentent 1 024
frames à 48 kHz. Aucun périphérique physique ni plugin installé n'est utilisé.
Les plugins sont les fixtures internes du dépôt. Il n'y a pas de nouvelle
comparaison JUCE, ASIO ou PortAudio/miniaudio dans cette campagne.

## Résultats des chemins préparés

Les durées couvrent le **groupe entier** indiqué : il ne faut pas les multiplier
à nouveau par le nombre de pistes ou d'instances.

| Groupe, 1 024 frames | µs par bloc du groupe | CPU normalisé, un cœur |
| --- | ---: | ---: |
| Adaptateur callback, sortie planaire | 0,156 | 0,00073 % |
| Adaptateur callback, sortie entrelacée | 1,672 | 0,00784 % |
| 32 transports mémoire, arrêtés | 3,122 | 0,01463 % |
| 32 transports mémoire, lecture en boucle | 7,038 | 0,03299 % |
| 8 instances VST3 internes, `process` direct | 20,988 | 0,09838 % |
| 16 compresseurs DSP publics | 1 025,281 | 4,80601 % |
| 16 chaînes compresseur + chorus + réverbération DSP | 1 744,025 | 8,17512 % |
| 8 synthétiseurs Core × 8 voix oscillateur | 373,869 | 1,75251 % |

Avec les diagnostics activés, l'adaptateur planaire coûte 0,191 µs et
l'entrelacé 1,706 µs. Ces appels incluent le remplissage de sortie et les
conversions éventuelles ; ils n'ouvrent aucun backend système. Les transports
lisent une source mémoire déjà préparée, sans fichier ni décodage. Les effets
utilisent les paramètres par défaut des processeurs DSP publics ; les
synthétiseurs ne reproduisent pas le graphe Ambre de CyDAW.

Le champ `projectedCorePercent48000` est une **normalisation** :
`durée_ns × (48 000 / frames) / 10 000 000`. Il suppose un appel du groupe par
bloc. Ce n'est pas une consommation CPU observée de CyDAW. Il exclut notamment
son graphe, ses copies supplémentaires, son ordonnanceur, ses fichiers, son UI
et son protocole IPC. La mesure du pump VST3 figure dans le JSON ; sa fréquence
réelle relève du thread de contrôle de l'application.

## Probe du buffer applicatif en lecture seule

`tests/DawBufferMeasurements.cpp` est un consommateur séparé du header
`cydaw/audio/SampleBuffer.h`. Aucun lien vers CyDAW n'est ajouté à la bibliothèque
Core ou à ses cibles installées. Le membre `VectorOps ops_` de ce header est
construit par défaut. Le backend scalaire par défaut est le contrat public de
CyDSP ; le probe le vérifie explicitement.

| Opération, groupe de 32 appels stéréo | Défaut applicatif µs | Variante préparée µs | Rapport |
| --- | ---: | ---: | ---: |
| Accumulation avec gain 1/32 | 3,966 | 2,619 | 1,51× |
| RMS des deux canaux | 34,894 | 1,838 | 18,99× |
| Copie puis gain constant 0,75 | 9,472 | 4,525 | 2,09× |

La variante crée ses `VectorOps` automatiques **avant** la mesure. Le RMS passe
de 0,16356 % à 0,00861 % d'un cœur normalisé, soit environ 0,155 point gagné.
Ce probe répète les opérations sur les mêmes plans en cache ; il ne simule pas
l'empreinte mémoire de 32 fichiers ou pistes distincts.
La dernière ligne remplace aussi une rampe de gain constant par `scale` : son
rapport combine ce choix d'algorithme et le dispatch, sans isoler leurs parts.
Les sorties de gain et de mix sont contrôlées échantillon par échantillon ; le
dot scalaire/automatique est comparé avec une tolérance relative de 1e-5.

Une adaptation applicative devrait préparer le backend sur contrôle et le
transmettre aux vues empruntées, dont la création peut avoir lieu sur audio.
Changer globalement le constructeur `VectorOps` ou découvrir le CPU dans chaque
vue audio serait inadapté au contrat de préparation. Aucune modification CyDAW
ou DSP n'est effectuée ici.

## Workers : résultat limité par le pilote simulé

Le banc Windows lance huit workers **Core** avec une fixture CLAP, puis observe
trois fenêtres de deux secondes : repos, `poll` demandé à 60 Hz et `process`
demandé à 48 000 / 1 024 Hz. Il distingue le CPU du parent et la somme des huit
enfants via `GetProcessTimes`, et mesure les durées individuelles des appels.

Les médianes CPU parent/enfants remontées sont nulles à cette résolution. Cela
**ne démontre pas un coût nul** : ces courtes fenêtres ne résolvent pas les
petites consommations. Le pilote `sleep_until` à priorité normale présente des
périodes observées d'environ 15 à 32 ms pour 21,333 ms demandées, et une latence
d'ordonnancement maximale proche de 16 ms. Les cinq essais conservent
respectivement 222, 223, 230, 252 et 274 indicateurs de blocs manqués, additionnés
sur les huit workers. Ce ne sont pas des xruns matériels et ce n'est pas une
validation des échéances ASIO. Les attentes appartiennent exclusivement au
pilote, hors des appels audio préparés. Les huit workers sont fermés après usage.

CyDAW possède son propre protocole dans `src/app/IsolatedPluginHost.cpp`.
Ces chiffres du `PluginWorker` Core ne mesurent pas cet IPC et ne justifient
aucune attribution du surcoût parent à l'un ou à l'autre.

## Protocole, preuves et limites

Cinq essais en processus neufs sont retenus. Chaque cas synchrone effectue
64 appels d'échauffement, puis 31 lots de 32 appels. Les tableaux donnent la
médiane des cinq médianes. Le `p95BatchMeanNs` des journaux bruts est un
percentile de moyennes de lots, jamais une borne de callback individuel. Les
statistiques `medianCallNs`/`p95CallNs` du worker portent, elles, sur des appels
individuels incluant huit instances.

L'ordre des programmes est alterné entre essais, ainsi que l'ordre des deux
variantes du probe applicatif. Les signaux et paramètres sont fixes ; les caches
sont chauds. Une signature issue des valeurs de sortie doit rester identique
entre processus pour un même cas. Elle ne constitue pas une preuve complète
d'équivalence numérique pour les effets ou plugins ; les régressions existantes
restent nécessaires.

Le pilote vérifie les empreintes Core/DSP et celles des headers empruntés avant
et après les essais. Il attend l'absence du compilateur/linker aux frontières et
rejette un essai si le jeu de processus CyDAW change ou si leur CPU observé
dépasse 1 % d'un cœur. Un CyDAW déjà ouvert est resté au-dessous de ce seuil.
Il n'est jamais lancé ou piloté par ce script. Ces contrôles n'éliminent pas
toute activité du système entre deux observations. Aucune affinité, priorité
temps réel, politique de fréquence ou résolution du timer n'est modifiée.

Les données de 128 frames, les cinq valeurs individuelles, les empreintes des
exécutables et les incidents du pilote restent dans le JSON associé. Les
sorties détaillées sont locales sous `build/core-path-profile`.

Les flags ASan optionnels des deux cibles de mesure ont été complétés après la
collecte. Le JSON distingue donc l'empreinte des entrées de compilation mesurées
de celle de la validation ASan finale. Les exécutables Release conservés sont
toujours identiques octet pour octet, empreintes vérifiées ; aucun source C++
mesuré n'a changé. Une relance ultérieure pendant l'activité de l'autre campagne
a été arrêtée après rejet des essais ; aucun de ces essais n'est ajouté aux
cinq essais valides présentés ici.

## Suite à mener dans la campagne CyDAW

1. Mesurer les piles CPU par thread du parent et les nombres d'appels, dans les
   scénarios exacts de la comparaison, au repos puis en lecture.
2. Tester la préparation explicite des buffers et le gain constant, puis
   comparer le CPU et les sorties du produit entier. Le facteur RMS isolé ne
   prédit pas le gain global.
3. Ventiler les coûts de `TrackMixer`, gain/pan, meters, `NativeDspGraph`,
   `BufferingAudioSource`, timers et `IsolatedPluginHost`. Rechercher les copies,
   préparations répétées ou interrogations trop fréquentes seulement si les
   traces et compteurs les mettent en évidence.

Les bancs actuels ne reproduisent pas le surcoût parent du produit. Ils
fournissent des coûts de référence et une piste concrète ; le diagnostic causal
global reste à confirmer dans le chat chargé de CyDAW.

## Reproduire

Les chemins ci-dessous sont relatifs à la racine Core ; `../CyDSP` et
`../CyDAW_Project` sont consultés en lecture seule. Aucune dépendance n'est
téléchargée. Le pilote crée une archive du commit DSP indiqué sous `build`.

```sh
# Construction seule : aucune campagne de chronométrage implicite.
python scripts/profile_core_paths.py

# Mesures Core et probe facultatif du header applicatif.
python scripts/profile_core_paths.py --daw-source ../CyDAW_Project --run --trials 5
```

`--dependency-commit` et `--cydsp-source` sélectionnent explicitement la référence
locale ; `--cmake` permet de préciser l'outil installé. Le pilote est prévu pour
Windows/MSVC avec les fixtures activées. Les cibles CMake
`cyaudio_path_measurements` et `cyaudio_worker_measurements` nécessitent
`CYAUDIOCORE_BUILD_PATH_MEASUREMENTS=ON` (OFF par défaut), Devices et Plugins.
Le passage du répertoire de fixtures à l'exécutable est explicite. Les fixtures
nécessitent `CYAUDIOCORE_BUILD_TESTS=ON`. La cible de mesure ne déclenche aucun
test ni exécution pendant une compilation ou installation normale.

## Validation exécutée

Les régressions complètes des services Core et la compilation indépendante des
headers publics passent sous Windows x64/MSVC Release, avec le DSP figé. Le
contrôle des sorties des nouveaux bancs passe sur les cinq essais. Les
validations de sources/provenance et de séparation des modules sont rejouées.

Les campagnes de temps utilisent des exécutables sans instrumentation. Les
exécutions AddressSanitizer sont des contrôles séparés de correction, sans
interprétation de leurs durées. Les régressions Core, les headers publics et les
trois nouveaux probes (chemins Core, workers et header applicatif) passent avec
AddressSanitizer ; la bibliothèque DSP liée est également instrumentée. Les
configurations PortAudio, consommateurs
installés et autres plateformes ne sont pas remesurées par cette livraison,
qui ne change aucune source de production ni export CMake. Les validations
antérieures restent décrites dans les rapports d'intégration correspondants.
