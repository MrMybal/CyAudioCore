# Prise en compte de l'extension CyDSP — 13 novembre 2026

La [référence CyDSP courante](CyDSPCurrent.fr.md) couvre les optimisations et la
reconstruction ultérieures. Ce document conserve le bilan de l'extension.

Révision examinée : `cc18dca043925b75adea481c1c8545a125607e1f`, après la
[précédente adaptation wavetable](CyDSPUpdate.md) à `6c48a3f`.
CyDSP conserve sa version 0.2.0 et ses cibles statiques `CyDSP::CyDSP` et
`CyDSP::Stretch`. Les ajouts n'imposent aucun changement aux API publiques
existantes de CyAudioCore. Recompiler les archives et consommateurs avec la même ABI.

## Ce qui change pour l'intégration

| Famille CyDSP | Raccordement à CyAudioCore et responsabilités de l'application |
| --- | --- |
| Granulator, LiveGranulator, SourceBatch | Employer les mêmes AudioBlock/SampleData/Wavetable ; conserver les banques empruntées jusqu'à l'arrêt des lecteurs. Le pool de grains n'a pas la politique de vol de voix du Synthesizer. |
| ModulatedOscillator, enveloppes, modèles physiques et ContactExciter | Les rendre par SynthVoiceRenderer, ou directement depuis un moteur préparé. Le synthétiseur reste responsable des événements MIDI/UMP, enveloppes de service et vols de voix. |
| LevelMeter, TruePeakMeter, SpectrumAnalyzer, ConstantQAnalyzer, LoudnessMeter, analyse musicale | Alimenter les vues produites par les sources, fichiers ou callbacks. Sérialiser les getters ; publier vers l'UI avec un échange préparé par l'application. Les calculs LUFS intégrés/LRA appartiennent au contrôle, hors callback critique. |
| MultiBusGraph, GraphTransition, QualityGraph | Définir explicitement les bus audio et sidechains. Déclarer les latences fixes des nœuds ; préparer/stager les changements hors callback. Conserver les contextes des deux graphes pendant les transitions et jusqu'à leur remplacement hors temps réel. |
| Dynamics avec détecteur externe, dynamique multibande, saturation, effets spectraux et restauration | Appeler directement leurs processeurs publics. Le nouveau sidechain peut être mono ou avoir autant de canaux que le programme ; aucune connexion implicite aux ports de plugins n'est ajoutée. |
| Acoustique, HRTF partagées, VBAP, Ambisonics/HOA | L'application déclare les positions et le sens de ses canaux. HOA utilise ACN/SN3D, jusqu'à 64 canaux ; une matrice discrète ne remplace pas un décodeur spatial. Aucun jeu HRTF ni chargeur SOFA n'est ajouté. |
| EchoCanceller, AdaptiveFilter | Fournir la référence et les microphones sur une horloge alignée. Les timestamps/latences périphériques peuvent rester inconnus ; ne pas inventer un délai de référence ni une correction de dérive. |
| Profils de qualité préparés | Choisir explicitement la politique à la préparation. Aucun réglage SIMD/FP global et aucun changement automatique de qualité ou de traitement existant. |

Les nouveaux traitements numériques restent dans le namespace `cydsp`.
`cyaudio` conserve les alias communs existants. Les headers publics CyDSP sont
utilisables avec les cibles CyAudioCore, sans recopier les algorithmes ni introduire
une deuxième famille de buffers. Les SDK de périphériques/plugins restent privés.
Les dépendances numériques et leurs notices restent celles de CyDSP ; aucun
nouveau composant tiers n'est importé dans CyAudioCore.

L'ancien chemin `Dynamics::process(program, output)` reste disponible. Son nouveau
chemin à détecteur externe ajoute une validation des signaux ; les données non
finies doivent donc être traitées comme une erreur. Le changement de Generators
factorise PolyBLEP dans CyDSP sans modifier l'API de nos voix. Wavetable,
SampleData, VectorOps, AudioBlock et les API de stretching utilisées ici ne
changent pas. L'adaptation wavetable par blocs précédente reste en place.

## Exemple de graphe sur un callback audio

[DspGraphCallback.h](../examples/DspGraphCallback.h) est un exemple d'application
testé, pas une nouvelle API installée. Il prépare les tables de pointeurs et le
silence, emprunte un graphe immobile et expose un callback float32 compatible
avec les deux fournisseurs. Les bus occupent les canaux consécutifs dans l'ordre
de déclaration ; par exemple stéréo programme + mono sidechain = trois entrées.
Le mapping vers les canaux natifs appartient toujours à AudioDeviceConfig.

```cpp
#include "DspGraphCallback.h" // Copier/adapter l'exemple dans l'application.

// Préparer le graphe et tous ses contextes sur le contrôle, avant cet appel.
cyaudio::examples::DspGraphCallback<float> engine;
auto status = engine.prepare(graph);
if (status != cyaudio::Status::ok) return status;
// config doit correspondre à la fréquence, au maximum de frames et à la somme
// des largeurs des bus du graphe. config.inputChannels=3 dans l'exemple ci-dessus.
status = audio.openWithContext(config, &decltype(engine)::callback, &engine);
```

Les objets `graph`, contextes des nœuds et `engine` vivent jusqu'à la fermeture
du périphérique. Le gestionnaire est préparé au préalable avec le fournisseur et
le backend choisis ; voir [l'exemple des fournisseurs](PortAudio.fr.md).
L'adaptateur commun découpe les callbacks natifs dépassant la capacité préparée.
L'exemple accepte les tailles variables dans cette capacité, les sous-blocs et les
entrées absentes. Il ne prépare ni ne détruit les graphes dans le callback.

`engine.timing()` décrit uniquement le DSP. Cette latence en frames ne modifie pas
les timestamps ADC/DAC et ne rend pas une latence périphérique inconnue connue.
Une modification dynamique de latence d'un plugin exige de reconstruire/stager
son graphe hors callback ; le worker conserve sa latence de pipeline documentée.
Les queues d'automation MIDI/plugin et les coûts propres aux plugins restent à
intégrer explicitement par l'application. Un callback tiers n'hérite pas de la
garantie temps réel des processeurs CyDSP.

## Vérifications et reproduction

Résultats exécutés sous Windows x64, MSVC 19.40, Release :

| Configuration / contrôle | Résultat |
| --- | --- |
| CyDSP cc18dca, PortAudio ON : régressions Core et DSP indépendantes | Réussi |
| CyDSP cc18dca, PortAudio OFF : régressions Core et DSP indépendantes | Réussi |
| CyDSP cc18dca, PortAudio ON + AddressSanitizer : régressions Core et DSP | Réussi |
| Headers publics indépendants Core et DSP | Réussi dans ces trois configurations |
| Consommateurs CMake installés puis déplacés, PortAudio ON/OFF | Réussi |
| Compatibilité avec la copie de CyDSP 6c48a3f, services en scalaire | Réussi ; tests d'extension explicitement ignorés |
| Empreintes de dépendance, frontières publiques et provenance | Réussi |

La copie de compatibilité est extraite du commit historique dans un répertoire
de build de CyAudioCore ; le dépôt CyDSP lui-même reste en lecture seule.
Les contrôles finaux portent sur les mêmes sources figées avant/après exécution.

Les régressions de composition testent les frontières entre bibliothèques :

- MemoryAudioSource vers graphe et LevelMeter : fin de source, silence, erreurs,
  reset, sous-blocs et échec de préparation sans perte de l'ancien état.
- Données de source vers SampleData/Granulator, avec offset porté entre blocs courts.
- SynthVoiceRenderer utilisant ModulatedOscillator : événements UMP aux offsets
  exacts, changement de pitch et relâchement, comparés au processeur public direct.
- SourceBatch partageant une wavetable vers graphe et sortie 64 canaux.
- Programme stéréo + sidechain mono vers Dynamics : buffers natifs entrelacés et
  planaires, mappings non triviaux, capture absente, sorties natives non utilisées
  silencieuses et callback de 67 frames découpé à une capacité de 17.
- Audits d'allocation/libération autour des traitements préparés et du reset.

Les tests de composition sont conditionnés à la présence des nouveaux headers.
Ils signalent explicitement leur absence avec un ancien package 0.2 ; les tests
existants et les API de production restent utilisables avec celui-ci.

Exécutions explicites, sans périphérique physique :

```sh
python scripts/run_native.py --portaudio --build-dir build/portaudio-on
python scripts/run_native.py --build-dir build/portaudio-off
python scripts/run_native.py --portaudio --sanitize --build-dir build/portaudio-asan
python scripts/validate_dsp_package.py --build-dir build/portaudio-on
python scripts/validate_dsp_package.py --build-dir build/portaudio-off
python scripts/test_reporting.py
python scripts/verify_sources.py
python scripts/verify_separation.py
```

Les rapports locaux `build/native-*.json` et `build/dsp-package-*.json` conservent
désormais l'empreinte des entrées de compilation CyAudioCore **et** CyDSP, la révision
de la dépendance si elle est disponible, et un contrôle de stabilité avant/après.
Un changement de sources pendant la validation fait échouer le rapport.
Les consommateurs installés puis déplacés vérifient les vrais packages résolus,
l'identité des types, le lien statique, graphes/qualité, granulation, analyse,
banques partagées et HRTF préparées en mémoire, sans header de fournisseur exposé.

Cette mise à jour ne mesure pas de nouveau gain de performance. Les
[mesures wavetable précédentes](Measurements.md) décrivent leur propre révision
et protocole ; elles ne sont pas extrapolées aux nouvelles fonctions.
La validation locale ne démontre ni comportement matériel, ni calibration AEC,
ni qualité perceptuelle universelle. Aucun périphérique audio/MIDI physique,
serveur JACK ou plugin tiers installé n'est ouvert. Les fixtures de périphériques
et de plugins restent déterministes. Aucun résultat d'exécution macOS, Linux,
Android, ARM64, x86 ou d'un autre compilateur n'est revendiqué par cette mise à jour.
La compatibilité scalaire historique ne constitue pas une nouvelle mesure du
backend scalaire de cc18dca. Aucun push ni GitHub Actions n'est lancé.
