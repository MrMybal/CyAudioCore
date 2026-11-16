# Référence CyDSP courante — 16 novembre 2026

Version intégrée et validée : **CyDSP 0.4.0**, révision
`c1d9a52fce7ac90515b0e264545903fdad2901b4`.
L’empreinte SHA-256 des entrées de compilation figées est
`ecaad142e90db85b7cfb590d6b07223f51a48eafdb62151b532ca7a6875be6b7`.
La référence précédente était CyDSP 0.3.2, commit
`0d67c6a60749ef241a87bc3592fa63c61998a5e2`.

## Intégration

- Les buffers, vues, conversions, `ProcessSpec`, `Status`, `VectorOps`, générateurs,
  enveloppes et stretching utilisés par Core conservent leurs signatures. Aucun
  noyau numérique n’est recopié et aucun flag SIMD global n’est ajouté.
- Les ajouts 0.4 comprennent les profils HRTF explicites, les délais fractionnaires,
  les HRIR analytiques possédés et les décodeurs HOA pondérés/MagLS. La disposition
  de types C++ publics comme `HrtfPreparation` évolue : reconstruire les archives,
  applications et workers avec les mêmes headers et la même copie de CyDSP.
- La latence et la traîne viennent du processeur choisi. Un rendu spatial ou un
  profil de qualité n’est jamais appliqué automatiquement par Core. La préparation
  HRTF/HOA reste hors du callback ; les vues et états empruntés gardent leur contrat
  de durée de vie.
- L’import SOFA depuis la mémoire et le cache d’assets HRTF sont des cibles séparées
  `CyDSP::Sofa` et `CyDSP::HrtfAssets`. `CYDSP_BUILD_SOFA` et
  `CYDSP_BUILD_HRTF_ASSETS` restent désactivés par défaut. L’application les lie
  explicitement. Le lecteur complet exige les choix HDF5/ZLIB explicites ; aucun
  SDK ou jeu de données HRTF n’est installé implicitement par Core.
- Les notices des composants SOFA restent distribuées par CyDSP. Son manifeste
  inventorie désormais sept composants, dont `libmysofa` et `miniz`. Les quatorze
  composants de services de Core et leurs notices sont inchangés. Le contrôle de
  séparation reconnaît les partitions 5/14 et 7/14, sans accepter d’autres noms.

## Versions CMake et ABI

Le minimum compatible reste **CyDSP 0.3.0**. La recherche d’un paquet installé
accepte les branches 0.4 et 0.3. Le checkout explicite
`CYAUDIOCORE_CYDSP_SOURCE_DIR` reste utilisable ; aucun téléchargement n’est ajouté.

À la génération, Core enregistre la version réelle du target CyDSP. Son paquet
installé expose `CyAudioCore_CYDSP_VERSION` et demande cette version **exacte**,
avec `Stretch` lorsque nécessaire. Un target CyDSP préchargé d’une autre version
est refusé avec un message demandant de reconstruire les archives et headers
correspondants. Ce contrôle de version ne remplace pas la cohérence des fichiers
pour deux états de développement portant le même numéro.

CyDSP a été consulté en lecture seule. Une copie de travail figée est conservée
sous `build/cydsp-update-0.4.0/source`, avec son empreinte dans le rapport local.
Les entrées de compilation de cette copie ont été comparées au checkout propre
du commit ci-dessus.
Les compilations et installations écrivent exclusivement dans le build de Core.

## Reconstruction

```sh
python scripts/build.py --preset windows-x64 --cydsp-source ../CyDSP --compile-checks
cmake --install build/windows-x64 --config Release --prefix build/install
```

Ces commandes prennent le checkout indiqué. Pour reproduire les contrôles décrits
ici, fournir la copie figée portant l’empreinte ci-dessus, puis reconstruire aussi
les applications et workers liés statiquement. miniaudio demeure le fournisseur
par défaut ; PortAudio reste optionnel.

## Contrôles

Le test de composition HRTF relie `MemoryAudioSource`, `AudioTransport`, le graphe
public CyDSP, son rendu binaural et l’adaptateur de callback Core en float et double.
Il compare au processeur direct les blocs variables, la fin de source, la traîne,
les changements de direction et le reset. Il contrôle les gardes de sous-blocs,
la propagation de latence et les allocations/libérations du chemin préparé.

Le consommateur CMake installé puis déplacé vérifie la version exacte et les
symboles 0.4, notamment la propriété et le déplacement des HRIR analytiques.
Il vérifie les deux ordres de chargement Core/CyDSP et le refus explicite d’un
ancien target préchargé.

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
| Paquets 0.4 installés puis déplacés, PortAudio ON/OFF | Réussi |
| Dépendance déjà chargée de même version / refus de 0.3.2 et 0.2.0 avec Core 0.4 | Réussi |
| Configuration de Core depuis les paquets installés 0.3.2 et 0.4.0 | Réussi |
| CyDSP 0.3.2 : composition, synthèse, sources, veille et consommateur installé | Réussi |
| Archives et worker de production reconstruits/installés ; tests/mesures OFF | Réussi |
| Sources, provenance et frontière publique pour 0.3.2 et 0.4.0 | Réussi |
| Manuel FR/EN régénéré, installation identique et quatre exemples C++ compilés | Réussi |

Les empreintes des entrées de compilation restent identiques pendant les campagnes.
Le build de production et `build/install` utilisent aussi la copie CyDSP 0.4.0.
MSVC conserve l’avertissement C4456 de CyDSP dans `AdvancedDynamics.cpp`
(variable locale `direct` masquée) ; ce fichier dépendant n’a pas été modifié.

Aucun périphérique physique n’est ouvert. Les tests indépendants de CyDSP ne sont
ni construits ni exécutés. Aucun benchmark n’est exécuté et aucun gain global
pour Core n’est déduit des optimisations ou profils spatiaux de CyDSP.
macOS, Linux, Android, x86, Windows ARM64 et SVE ne sont pas exécutés pour cette
mise à jour. Les lecteurs SOFA optionnels et les jeux de données matériels ne sont
pas validés par les tests de composition du cœur.
