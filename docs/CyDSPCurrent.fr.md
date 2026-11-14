# Référence CyDSP courante — 15 novembre 2026

Version intégrée et validée : **CyDSP 0.3.2**, révision
`0d67c6a60749ef241a87bc3592fa63c61998a5e2`. Cette mise à jour succède à
`e3d719f8f8753a4090ee75c2df2e399c0778dea9` (0.3.0) et inclut 0.3.1.
L’empreinte SHA-256 des entrées de compilation de CyDSP est
`114977a9b190a6dd11a017bddf6c144cbc5d69230976cb0ef419782ec3a73bd7`.

## Changements de la dépendance

- 0.3.1 partage les courbes de saturation et de dynamique entre AVX2, AVX-512,
  NEON ARM64 et SVE. Les travaux FFT progressifs et le recouvrement des partitions
  de convolution sont regroupés dans les budgets existants.
- 0.3.2 fusionne le drive, le wavefolder direct et leur mixage, met en cache la
  compensation DC, vectorise une partie du transient précis et traite l’EQ
  dynamique par paires de canaux indépendants. Les qualités par défaut restent
  inchangées ; le calcul précis conserve ses appels scalaires à `std::pow`.
- La convolution copie les entrées seulement lorsqu’elles partagent la mémoire
  des sorties. Les canaux routés sont réécrits intégralement ; les autres sont
  effacés. Les publications des recouvrements utilisent les primitives SIMD.
- Les signatures publiques utilisées par Core restent compatibles. La disposition
  privée de `MatrixConvolver` change : reconstruire les archives et les
  consommateurs avec les mêmes headers. Remplacer seulement une archive dans
  une application déjà compilée n’est pas une mise à jour d’ABI valide.

Ces optimisations restent dans CyDSP. Core ne recopie aucun noyau et n’ajoute
aucun flag SIMD ou fast-math global. Les effets sont choisis et préparés par
l’application ; Core ne les applique pas automatiquement à un flux.

## Intégration et version minimale

Les buffers, vues, conversions, `ProcessSpec`, `Status`, `VectorOps`, files,
générateurs et traitements de stretching employés par Core ne nécessitent pas de
modification de leur API. Le synthétiseur conserve `Adsr::process` et
`SampleData::render`, introduits en 0.3.0, avec segmentation MIDI/UMP.

Le minimum compatible reste **CyDSP 0.3.0**. CMake et le garde de compilation
continuent de demander la branche d’API 0.3 ; 0.3.2 est la révision de validation,
pas une dépendance téléchargée implicitement. CyDSP reste un projet externe,
utilisé via `CYAUDIOCORE_CYDSP_SOURCE_DIR` ou un paquet installé. Il n’est pas
copié dans les sources distribuées de Core.

Le checkout CyDSP a été consulté en lecture seule. La campagne utilise une archive
immuable du commit ci-dessus, créée sous `build/cydsp-update-0.3.2`, pour ne pas
être affectée par une modification concurrente du dépôt dépendant.

## Reconstruction

```sh
python scripts/build.py --preset windows-x64 --cydsp-source ../CyDSP --compile-checks
cmake --install build/windows-x64 --config Release --prefix build/install
```

Ces commandes compilent le checkout fourni ; pour reproduire cette campagne,
fournir une archive de la révision exacte ci-dessus avec `--cydsp-source`.
Reconstruire aussi les applications et workers qui lient ces archives statiques.
Les options de fournisseurs audio et de worker restent des choix CMake explicites.

## Validation de l’intégration

La suite `dspComposition` relie désormais `MemoryAudioSource`, `AudioTransport`,
le graphe public CyDSP, l’adaptateur de callback Core et `MatrixConvolver`, en
float et double. Elle compare un routage partiel à des signaux attendus avec des
blocs de 1 à 64 frames, en modes hybrid/nonuniform/scheduled et avec tête spectrale
explicite. Elle vérifie les sorties sans route, la traîne après fin de lecture,
les buffers distincts et en place, les sous-blocs protégés et le reset. Les appels
préparés passent dans l’audit d’allocation/libération des tests Core.

Le consommateur installé de `scripts/validate_dsp_package.py` demande la version
exacte du paquet sélectionné et compare les trois composantes de `Version.h`.
Il construit, traite et détruit aussi un `MatrixConvolver` contre l’archive
installée, en plus des API de synthèse, d’analyse et de qualité déjà couvertes.
Le paquet et le consommateur sont déplacés pour vérifier la résolution CMake
indépendamment du checkout et du premier préfixe d’installation.

```sh
python scripts/run_native.py --services-only --portaudio --cydsp-source ../CyDSP
python scripts/run_native.py --services-only --portaudio --sanitize --cydsp-source ../CyDSP
python scripts/run_native.py --services-only --cydsp-source ../CyDSP
python scripts/validate_dsp_package.py --build-dir build/native-x64-portaudio --cydsp-source ../CyDSP --require-prepared-optimizations
```

Windows x64, MSVC 19.40, archives Release :

| Contrôle | Résultat |
| --- | --- |
| Régressions Core avec PortAudio simulé (27 suites) | Réussi |
| Régressions Core avec AddressSanitizer, DSP instrumenté, PortAudio simulé (27 suites) | Réussi |
| Régressions Core sans PortAudio (26 suites) | Réussi |
| Compilation indépendante des headers publics Core, dans les trois configurations | Réussi |
| Reconstruction et installation de production, tests et mesures désactivés, worker explicitement activé | Réussi |
| Consommateurs installés puis déplacés, PortAudio ON/OFF, paquet et headers 0.3.2 exacts | Réussi |
| Contrôles des sources, provenance et frontière publique Core/DSP | Réussi |

Les entrées de compilation sont identiques avant et après chaque campagne. Le
build principal et `build/install` utilisent aussi la copie figée de CyDSP 0.3.2 ;
les périphériques de production conservent miniaudio par défaut.
MSVC signale l’avertissement C4456 dans `AdvancedDynamics.cpp` de CyDSP (variable
locale `direct` masquée). Ce fichier dépendant n’a pas été modifié.

Aucun périphérique physique n’est ouvert. La suite DSP indépendante n’est ni
construite ni lancée ; les tests natifs sont ceux de Core et de ses consommateurs.
Aucun benchmark comparatif n’est exécuté et aucun gain global pour Core n’est
déduit des mesures réalisées dans CyDSP. Les mesures historiques de Core
conservent leurs révisions de référence dans [Measurements.md](Measurements.md).

macOS, Linux, Android, x86, Windows ARM64 et SVE ne sont pas exécutés pour cette
mise à jour. Leur couverture publiée dans CyDSP ne remplace pas une validation de
Core sur ces cibles. Les SDK et notices des dépendances privées sont inchangés.
