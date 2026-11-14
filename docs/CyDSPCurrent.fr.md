# Référence CyDSP courante — 14 novembre 2026

Version intégrée et validée : **CyDSP 0.3.0**, construite sur la révision
`04f94e29ee15d98567fcbffc3f88045082ae9279` (noyaux par tuiles, ADAA exact,
`spectralHead`, FFT fusionnée). Au moment de cette validation, les modifications
0.3.0 étaient présentes dans le checkout local `../CyDSP` sans commit ; la révision
définitive est à reporter ici une fois ce commit créé. Le bilan de la révision
précédente `b973c1b` reste dans l'historique de ce fichier.

## Ce qui change en 0.3.0

- Le numéro de paquet suit désormais l'API. Il était resté à 0.2.0 malgré les
  extensions successives. Le paquet n'accepte qu'une même version mineure :
  CyAudioCore demande `find_package(CyDSP 0.3)`.
- `cydsp/core/Version.h` donne la version à la compilation. `DspTypes.h` la vérifie
  par `static_assert`, car un checkout fourni par `CYAUDIOCORE_CYDSP_SOURCE_DIR`
  ne passe pas par le contrôle de version de `find_package`.
- `Adsr::process` et `SampleData::render` sont des formes par bloc qui renvoient
  exactement les valeurs de `next` et de `value`. `SpscQueue` avance ses index sans
  division entière.

## Adoption dans Core

Le synthétiseur appelle `Adsr::process` une fois par voix et par segment délimité
par les événements MIDI/UMP, pour les trois sources (oscillateur, wavetable,
échantillonneur). L'échantillonneur appelle `SampleData::render` : le niveau mip est
choisi une fois par segment, le bouclage et le fondu de boucle sont traités par
CyDSP. La première frame située après la fin de l'échantillon relâche toujours la
note avant sa valeur d'enveloppe. Aucun algorithme numérique n'est recopié dans Core.
Les [mesures](Measurements.md) donnent les temps avant/après ; les sommes de
contrôle sont identiques dans les 36 cas mesurés.

Les files de la lecture anticipée, de l'enregistreur, du transport et du collecteur
MIDI utilisent la `SpscQueue` mise à jour sans changement de code.

## Dépendance et reconstruction

CyDSP est une dépendance externe de CyAudioCore. CMake utilise soit le répertoire
fourni par `CYAUDIOCORE_CYDSP_SOURCE_DIR`, soit un package installé `CyDSP 0.3`.
Les presets locaux désignent `../CyDSP`. Changer ce dépôt ne met pas à jour une
archive statique déjà construite : il faut reconstruire Core, le DSP lié et les
consommateurs qui les embarquent.

```sh
python scripts/build.py --preset windows-x64 --cydsp-source ../CyDSP --compile-checks
cmake --install build/windows-x64 --config Release --prefix build/install
```

Le build principal Windows x64 a été reconstruit avec ce checkout, puis installé
sous `build/install` (paquets `CyDSP 0.3.0` et `CyAudioCore 0.1.0`).

## Compatibilité des API et de l'ABI

Les types AudioBlock/AudioBuffer/ProcessSpec/Status/VectorOps, les générateurs,
wavetables et traitements de stretching employés dans Core restent compatibles au
niveau source. Les headers et archives doivent être reconstruits ensemble :
`ConvolutionConfig` gagne le champ `spectralHead`, `MatrixConvolver` et `RealFft`
changent de taille, `Adsr` et `SampleData` gagnent des méthodes. Un consommateur
qui demandait `find_package(CyDSP 0.2)` doit demander 0.3 et être recompilé.

Les nouveautés de `04f94e2` (tuiles de saturation et de dynamique, `spectralHead`,
FFT fusionnée) concernent les effets d'une application : Core ne les sélectionne ni
ne les applique. Aucun flag SIMD ou fast-math global n'est ajouté.

Le consommateur installé de `scripts/validate_dsp_package.py` vérifie désormais la
version 0.3 des headers installés et lie `Adsr::process` et `SampleData::render`
dans la véritable archive DSP :

```sh
python scripts/validate_dsp_package.py --build-dir build/windows-x64 --cydsp-source ../CyDSP --require-prepared-optimizations
```

## Validation

Windows x64, MSVC 19.40, Release :

| Contrôle | Résultat |
| --- | --- |
| Build complet à froid, PortAudio ON, compilation indépendante des headers Core et DSP | Réussi, aucun avertissement |
| Régressions complètes Core, PortAudio ON (25 suites) | Réussi |
| Régressions Core avec AddressSanitizer, DSP instrumenté, PortAudio ON | Réussi |
| Régressions DSP indépendantes, Release | Réussi |
| Suite DSP indépendante avec AddressSanitizer | Non exécutée : binaire supprimé par l'antivirus local |
| Build principal statique de production et installation | Réussi, aucun avertissement |
| Consommateurs installés puis déplacés, PortAudio ON et OFF, API 0.3 et symboles préparés | Réussi |
| Contrôles de sources et de provenance des deux dépôts | Réussi |

Non exécutés pour cet état : builds scalaire, x86, clang-cl, Windows ARM64, Android,
macOS et Linux. Les régressions de périphériques utilisent les fixtures
simulées/nulles ; aucun périphérique physique ni serveur JACK n'est ouvert.

Le pilote `profile_core_paths.py` sélectionne le HEAD local de CyDSP par défaut,
puis en crée une archive immuable sous `build`. Pour reproduire une ancienne
campagne, passer sa révision explicitement avec `--dependency-commit`.
Les [mesures de diagnostic](CorePathProfiling.fr.md) et les
[mesures du correctif transport](TransportOutputOptimization.fr.md) conservent
leur référence historique `fda6bd3` et leurs résultats d'origine.
