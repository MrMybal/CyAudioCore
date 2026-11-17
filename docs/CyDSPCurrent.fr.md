# Référence CyDSP courante — 17 novembre 2026

Version intégrée et validée : **CyDSP 0.4.5**, révision
`68a423a5f97338697b97b3af7d59d735c961fc9d`.
L’empreinte SHA-256 des entrées de compilation figées est
`1c6e62fa5015dd179436799cea7bd306b007f9c8f641543a69367c1d4a5bd279`.
La référence précédente était CyDSP 0.4.0, commit
`c1d9a52fce7ac90515b0e264545903fdad2901b4`.

## Intégration

Les buffers, vues, conversions, `ProcessSpec`, `Status`, `VectorOps`, générateurs,
enveloppes et stretching utilisés par Core conservent leurs signatures. Aucun
noyau numérique n’est recopié et aucun flag SIMD global n’est ajouté.

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
La disposition de types C++ publics évolue : reconstruire les archives,
applications et workers avec les mêmes headers et la même copie de CyDSP.

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
correspondants, y compris entre 0.4.0 et 0.4.5. Ce contrôle ne remplace pas la
cohérence des fichiers pour deux états de développement de même numéro.

CyDSP est consulté en lecture seule. La copie figée se trouve sous
`build/cydsp-update-0.4.5/source`. Ses entrées de compilation correspondent au
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

Le consommateur installé puis déplacé vérifie les symboles de timing,
d’interpolation et de rendu HRTF, ainsi que le lien des nouveaux types neuronaux.
Il contrôle les deux ordres d’import des paquets et le refus d’un target 0.4.0
préchargé. Ce contrôle de lien ne valide pas l’inférence d’un modèle entraîné.

Les commandes de validation restent explicites :

```sh
python scripts/run_native.py --services-only --portaudio --cydsp-source ../CyDSP
python scripts/run_native.py --services-only --portaudio --sanitize --cydsp-source ../CyDSP
python scripts/run_native.py --services-only --cydsp-source ../CyDSP
python scripts/validate_dsp_package.py --build-dir build/native-x64-portaudio --cydsp-source ../CyDSP --require-prepared-optimizations
```

Windows x64, MSVC 19.40, builds Release :

| Contrôle | Résultat |
| --- | --- |
| Core avec PortAudio simulé, 28 suites | Réussi |
| Core + CyDSP instrumentés AddressSanitizer, PortAudio simulé, 28 suites | Réussi |
| Core sans PortAudio, 27 suites | Réussi |
| Compilation indépendante des headers publics, trois configurations | Réussi |
| Paquets 0.4.5 installés puis déplacés, PortAudio ON/OFF | Réussi |
| Dépendance préchargée identique / refus de 0.4.0 avec Core 0.4.5 | Réussi |
| Configuration de Core depuis le paquet CyDSP 0.4.5 installé | Réussi |
| Archives et worker de production reconstruits/installés ; tests/mesures OFF | Réussi |
| Sources, provenance et frontière publique 7/14 | Réussi |
| Manuel FR/EN régénéré ; installation identique | Réussi |

Les empreintes des entrées restent stables pendant les campagnes. Le build de
production et `build/install` utilisent aussi la copie CyDSP 0.4.5.

Aucun périphérique physique n’est ouvert. Les tests indépendants de CyDSP ne sont
ni construits ni exécutés. Aucun benchmark n’est exécuté et aucun gain global
pour Core n’est déduit des optimisations spatiales. Les scénarios perceptuels,
poids neuronaux entraînés, lecteurs SOFA et mesures SADIE II optionnels ne sont
pas validés par cette campagne. macOS, Linux, Android, x86, Windows ARM64 et SVE
ne sont pas exécutés pour cette mise à jour. La compatibilité 0.3 conserve ses
contrôles antérieurs ; elle ne constitue pas une nouvelle exécution ici.
