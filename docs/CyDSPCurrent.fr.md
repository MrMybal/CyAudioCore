# Référence CyDSP courante — 14 novembre 2026

Révision intégrée et validée : **`ea56508ad3e26d790e491140b7f18a95e4411d5d`**,
package **CyDSP 0.2.0**. Elle comprend les optimisations récentes des courbes de
dynamique, saturations ADAA, banques de filtres, conception des coefficients et
convolution matricielle. Le code numérique et ses noyaux SIMD restent dans CyDSP.

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
Les headers et archives doivent toutefois être reconstruits ensemble.

L'API publique `Dynamics::minimumAppliedGain()` est disponible dans cette
révision. Elle donne le minimum de gain du détecteur, makeup inclus, avant le
plafond du limiteur, pour le dernier bloc non vide traité avec succès. Elle
permet à une application d'éviter des divisions sortie/entrée pour son indicateur
de réduction. L'accès reste sérialisé par le propriétaire du processeur.

Le consommateur CMake peut exiger explicitement ce symbole récent :

```sh
python scripts/validate_dsp_package.py --build-dir build/windows-x64 --cydsp-source ../CyDSP --require-current-dynamics
```

Le contrôle compile le header installé, lie la méthode dans la véritable archive
DSP, puis vérifie un rendu transparent et son gain. Il détecte notamment une
installation mêlant un header récent à une ancienne archive dépourvue du symbole.

## Validation

Windows x64, MSVC 19.40, Release :

| Contrôle | Résultat |
| --- | --- |
| Build principal statique, compilation indépendante des headers Core | Réussi |
| Régressions complètes Core et DSP indépendantes, PortAudio ON | Réussi |
| Mêmes régressions avec AddressSanitizer | Réussi |
| Headers publics indépendants Core et DSP dans les builds de validation | Réussi |
| Consommateurs installés puis déplacés, PortAudio ON et OFF | Réussi |
| Nouveau symbole Dynamics et rendu vérifiés dans ces deux consommateurs | Réussi |

Les contrôles indépendants emploient une archive locale figée du même commit ;
les sources et headers sont identiques à ceux du checkout utilisé pour le build
principal. Seul `CMakeLists.txt` diffère en fins de ligne CRLF/LF ; son contenu
normalisé est identique. Les empreintes brutes distinctes restent dans les rapports.
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
