# Référence CyDSP courante — 14 novembre 2026

Révision intégrée et validée : **`b973c1bc0aaf7a7db26a150af7cd6a2ad4e42ee5`**,
package **CyDSP 0.2.0**. Elle ajoute les optimisations de FFT, les calculs préparés
pour l'automation et la spatialisation, les traitements avec mixage intégré et
des choix explicites de précision. Elle comprend aussi les optimisations de la
révision précédente `ea56508`. Le code numérique et ses noyaux SIMD restent dans CyDSP.

## Dépendance et reconstruction

CyDSP est une dépendance externe de CyAudioCore. CMake utilise soit le
répertoire fourni par `CYAUDIOCORE_CYDSP_SOURCE_DIR`, soit un package installé
`CyDSP 0.2`. Les presets locaux désignent `../CyDSP`. Changer ce dépôt ne met pas
à jour une archive statique déjà construite : il faut reconstruire Core, le DSP
lié et les consommateurs qui les embarquent.

Le build principal Windows x64 a été reconstruit avec le checkout local courant,
puis installé sous `build/install`. Les archives installées sont comparées octet
pour octet aux archives produites. Les empreintes et la révision locale sont
conservées sous `build/windows-x64/dependency-update.json`.

```sh
python scripts/build.py --preset windows-x64 --cydsp-source ../CyDSP --compile-checks
cmake --install build/windows-x64 --config Release --prefix build/install
```

`--cydsp-source` choisit explicitement le checkout local et remplace le choix du
preset. Le script garde les tests et mesures désactivés en production et
normalise l'environnement Windows transmis à CMake/MSBuild. Il ne télécharge ni
ne modifie la dépendance. Le fournisseur audio par défaut reste miniaudio.

## Compatibilité des API

Les types AudioBlock/AudioBuffer/ProcessSpec/Status/VectorOps, les générateurs,
wavetables et traitements de stretching employés dans Core restent compatibles.
Aucune adaptation des traitements de production Core ni hausse de la version
minimale du package n'est nécessaire. Le correctif du transport Core reste inclus.
Les headers et archives doivent toutefois être reconstruits ensemble :
`QualitySettings`, `SaturationParameters` et `TransientParameters` ont de nouveaux
champs. Les consommateurs de ces structures doivent également être recompilés.

Les FFT optimisées sont utilisées par les plans DSP existants. Les nouvelles API
`Dynamics::processMixed`, `AdaaSaturator::processMixed`,
`TransientShaper::processGain`, `PeakingBiquadDesigner`, `DopplerPlan` et les
matériaux acoustiques préparés sont disponibles via les headers publics de CyDSP.
Core n'implémente pas de doublons ; leur adoption dans les effets d'une application
reste à faire dans cette application.

Les processeurs conservent `MathAccuracy::precise` et l'ADAA de saturation par
défaut. Le profil DSP `economy`, choisi explicitement puis appliqué par l'hôte,
propose désormais `MathAccuracy::fast` pour saturation et transitoires, et
`saturationAntialias=false`. Les profils `realtimeHigh` et `offline` conservent
la précision et l'ADAA. CyAudioCore ne sélectionne ni n'applique automatiquement
ces nouvelles options. Aucun flag SIMD ou fast-math global n'est ajouté.

L'API publique `Dynamics::minimumAppliedGain()` est disponible dans cette
révision. Elle donne le minimum de gain du détecteur, makeup inclus, avant le
plafond du limiteur, pour le dernier bloc non vide traité avec succès. Elle
permet à une application d'éviter des divisions sortie/entrée pour son indicateur
de réduction. L'accès reste sérialisé par le propriétaire du processeur.

Le consommateur CMake peut exiger les symboles de cette nouvelle révision :

```sh
python scripts/validate_dsp_package.py --build-dir build/windows-x64 --cydsp-source ../CyDSP --require-prepared-optimizations
```

Le contrôle compile les headers installés, lie les méthodes dans la véritable
archive DSP, puis vérifie les rendus Dynamics et saturation avec mixage, le gain
du détecteur de transitoires, la conception préparée d'un filtre et le Doppler
neutre. Il vérifie aussi les réglages précis et les réglages du profil economy.
Il détecte notamment une installation mêlant un header récent à une ancienne
archive dépourvue des symboles. L'option précédente `--require-current-dynamics`
reste utilisable seule pour le contrôle limité au compteur Dynamics.

## Validation

Windows x64, MSVC 19.40, Release :

| Contrôle | Résultat |
| --- | --- |
| Build principal statique, compilation indépendante des headers Core | Réussi |
| Régressions complètes Core et DSP indépendantes, PortAudio ON | Réussi |
| Régressions Core avec AddressSanitizer, DSP instrumenté | Réussi |
| Suite DSP indépendante avec AddressSanitizer | Non exécutée : binaire supprimé par l'antivirus local |
| Headers publics indépendants Core et DSP dans les builds de validation | Réussi |
| Consommateurs installés puis déplacés, PortAudio ON et OFF | Réussi |
| Nouveaux symboles préparés/mixage, rendus et politiques de précision dans les deux consommateurs | Réussi |

La compilation instrumentée complète réussit et les régressions Core passent.
La suite DSP indépendante instrumentée n'a pas pu démarrer : l'antivirus local
supprime son exécutable `cydsp_tests.exe`, ce que le propriétaire a confirmé.
Les deux tentatives sont conservées sous `build/cydsp-update-b973c1b` et le
rapport global ASan garde son échec de lancement. Cette suite reste non exécutée.

Les contrôles indépendants emploient une archive locale figée du même commit ;
l'empreinte des entrées compilées correspond à celle du checkout utilisé pour
le build principal. Les empreintes et l'inventaire des différences de fins de
ligne restent dans les rapports sous `build/cydsp-update-b973c1b`.
Les régressions de périphériques utilisent les fixtures
simulées/nulles. Aucun périphérique physique ni serveur JACK n'est ouvert.
Les autres plateformes et un nouveau gain de performance global ne sont pas
revendiqués par cette mise à jour.

Le pilote `profile_core_paths.py` sélectionne désormais le HEAD local de CyDSP
par défaut, puis en crée une archive immuable sous `build`. Pour reproduire une
ancienne campagne, passer sa révision explicitement avec `--dependency-commit`.
Une nouvelle construction archive les anciens résultats avant d'écrire ses
entrées ; une construction seule ne laisse pas d'anciens temps dans le rapport
courant.
Les [mesures de diagnostic](CorePathProfiling.fr.md) et les
[mesures du correctif transport](TransportOutputOptimization.fr.md) conservent
leur référence historique `fda6bd3` et leurs résultats d'origine.
