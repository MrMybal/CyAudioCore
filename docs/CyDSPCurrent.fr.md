# Référence CyDSP courante — 18 novembre 2026

CyAudioCore **0.2.0** utilise **CyDSP 0.4.6**, révision
`4963fec91ffa4d4a8e60053edafa9070b3ecb6e5`.
L’empreinte SHA-256 des entrées de compilation figées est
`541186f2d5867298d560e4f40dba27d4e185e82e130b29ac2f9c8ff9ea419e0b`.
La référence précédente était CyDSP 0.4.5, commit
`68a423a5f97338697b97b3af7d59d735c961fc9d`.

## Changements entre 0.4.5 et 0.4.6

Les signatures publiques utilisées par Core restent compatibles. Pour les applications
qui relient leurs processeurs au transport ou au callback de Core, trois changements
concernent directement les buffers partagés et le rendu par blocs.

- `SourceBatch` lit correctement les grains à vitesse négative.
- `ModulatedOscillator` détecte le chevauchement des canaux de sortie même en
  l’absence de modulation et préserve son état pour le bloc suivant.
- `MultiBusGraph` et `GraphTransition` sélectionnent exactement le signal voulu
  aux extrémités du fondu, même si l’autre signal contient une valeur non finie.

`MusicalAnalyzer` prépare les poids non nuls des bandes Mel et les cosinus
utilisés pour calculer les MFCC. Les mesures publiées par CyDSP portent sur
cet analyseur. [Mesures de CyDSP à la révision intégrée](https://github.com/MrMybal/CyDSP/blob/4963fec91ffa4d4a8e60053edafa9070b3ecb6e5/docs/Measurements.md)

## Intégration HRTF depuis CyDSP 0.4.5

`HrtfPreparation::removeCommonDelay` retire, sur choix explicite, le délai
acoustique minimal de toute la banque. Le défaut conserve le timing mesuré.
`HrtfSet::removedCommonDelayFrames()` expose l’avance au taux cible : elle reste
distincte de la latence du processeur et conserve les écarts entre oreilles et
directions. Le délai de conversion de fréquence n’est pas retiré.

`HrirRenderConfig` choisit une convolution directe, FFT ou automatique à la
préparation. L’interpolation est disponible via l’API publique `HrtfSet`.
Le rendu FFT n’ajoute pas de latence de bloc. Le graphe reçoit toujours la
latence et la traîne annoncées par le processeur choisi. Les rapports HOA
ajoutent des énergies en champ diffus et un gain de compensation proposé ;
Core n’applique pas ce gain implicitement.

L’API expérimentale `NeuralHrtfModel`/`NeuralBinaural` demeure dans CyDSP.
Core ne charge pas de poids et n’active pas de rendu automatiquement. Le modèle
doit correspondre au taux du rendu. Les mises à jour de design, leur rejeu
borné d’historique et les traitements groupés restent des choix applicatifs.
Reconstruire les archives, applications et workers avec les mêmes en-têtes et la
même copie de CyDSP.

## Dépendances optionnelles

L’import SOFA et le cache d’assets HRTF sont des cibles séparées `CyDSP::Sofa`
et `CyDSP::HrtfAssets`. `CYDSP_BUILD_SOFA` et `CYDSP_BUILD_HRTF_ASSETS` restent
désactivés par défaut. L’application les lie explicitement. Le lecteur complet
exige les choix HDF5/ZLIB explicites.

La sélection SADIE II fournie par CyDSP reste séparée du code MIT et des archives
statiques. Son installation Apache-2.0 avec notices se choisit par
`CYDSP_INSTALL_HRTF_DATA=ON`, désactivé par défaut. `CyDSP_HRTF_DATA_DIR` indique
le répertoire installé lorsqu’il existe. Core n’installe ni mesures ni modèles
entraînés implicitement ; aucun téléchargement n’est ajouté à CMake.

Le manifeste numérique de CyDSP conserve ses sept composants, dont `libmysofa`
et `miniz`. Les notices et le manifeste des données HRTF restent dans CyDSP.
Les quatorze composants de services et leurs notices dans Core sont inchangés.

## Versions CMake et ABI

Le minimum compatible reste **CyDSP 0.3.0**. La recherche d’un paquet installé
accepte les branches 0.4 et 0.3. Le checkout explicite
`CYAUDIOCORE_CYDSP_SOURCE_DIR` reste utilisable.

À la génération, Core enregistre la version réelle du target CyDSP. Son paquet
installé expose `CyAudioCore_CYDSP_VERSION` et demande cette version **exacte**,
avec `Stretch` lorsque nécessaire. Un target préchargé d’une autre version est
refusé avec un message demandant de reconstruire les archives et headers
correspondants, y compris entre 0.4.5 et 0.4.6. Ce contrôle ne remplace pas la
cohérence des fichiers pour deux états de développement de même numéro.

CyDSP est consulté en lecture seule. La copie figée se trouve sous
`build/cydsp-update-0.4.6/source`. Ses entrées de compilation correspondent au
checkout propre du commit indiqué. Compilations et installations écrivent dans
le build de Core.

## Reconstruction

```sh
python scripts/build.py --preset windows-x64 --cydsp-source ../CyDSP --compile-checks
cmake --install build/windows-x64 --config Release --prefix build/install
```

Ces commandes utilisent le checkout indiqué. Pour reproduire les contrôles
ci-dessous, fournir la copie figée portant l’empreinte indiquée, puis reconstruire
les applications et workers liés statiquement. miniaudio demeure le fournisseur
par défaut ; PortAudio reste optionnel.

## Contrôles

Le test de composition relie `MemoryAudioSource`, `AudioTransport`, le graphe
public CyDSP, son rendu binaural et l’adaptateur de callback Core en float et double.
Il compare les chemins direct/FFT à un processeur direct avec délai conservé ou
retiré explicitement : blocs variables, fin de source, traîne, directions, reset,
gardes de sous-blocs et propagation de latence. Le chemin préparé est audité pour
les allocations et libérations.

Le test de bypass 0.4.6 relie une source stéréo en mémoire à un effet dont la
sortie contient des NaN et dont la latence déclarée est de deux échantillons.
En bypass complet, le callback restitue exactement les deux canaux de la source
avec ce décalage. Le test vérifie chaque échantillon sur deux blocs, y compris
le silence après la fin de la source, en `float` et en `double`.

Le consommateur installé puis déplacé vérifie les symboles de timing,
d’interpolation et de rendu HRTF, ainsi que le lien des nouveaux types neuronaux.
Il contrôle les deux ordres d’import des paquets. L’option
`--incompatible-dsp-prefix` vérifie le rejet d’une autre version déjà chargée.

Les commandes de validation restent explicites :

```sh
python scripts/run_native.py --services-only --portaudio --cydsp-source ../CyDSP
python scripts/run_native.py --services-only --portaudio --sanitize --cydsp-source ../CyDSP
python scripts/run_native.py --services-only --cydsp-source ../CyDSP
python scripts/validate_dsp_package.py --build-dir build/native-x64-portaudio --cydsp-source ../CyDSP --require-prepared-optimizations
```

### Résultats avec CyAudioCore 0.2.0 et CyDSP 0.4.6

La campagne du 18 novembre 2026 utilise Windows x64, MSVC 19.40, des
périphériques virtuels et l’API simulée de PortAudio.

| Contrôle | Résultat |
| --- | --- |
| Régressions Core en Release avec PortAudio | 28 suites réussies sur 28 |
| Régressions Core avec AddressSanitizer, Core et CyDSP instrumentés, PortAudio activé | 28 suites réussies sur 28 |
| Régressions Core en Release sans PortAudio | 27 suites réussies sur 27 |
| Compilation indépendante des en-têtes publics de Core dans les trois configurations | Réussie |
| Compilation et exécution d’une application avec les paquets installés puis déplacés, avec et sans PortAudio | Réussies |
| Résolution CMake avec CyDSP préchargé | 0.4.6 accepté, 0.4.5 rejeté |
| Requête CMake pour Core 0.1 sur l’installation 0.2.0 | Rejetée |
| Configuration de Core avec le paquet CyDSP 0.4.6 installé | Réussie |
| Installation des archives statiques de production et du worker, tests et mesures désactivés | Réussie |
| Vérification des sources, de la provenance des 14 composants de Core et de la séparation avec les 7 composants de CyDSP | Réussie |
| Export CyDoc français et anglais | Manuels installés identiques à l’export |

Les rapports de la campagne et l’empreinte du checkout sont conservés sous
`build/cydsp-update-0.4.6`. Les suites exécutées sont celles de CyAudioCore.
Les tests autonomes de CyDSP et les benchmarks sont hors du périmètre de cette campagne.
