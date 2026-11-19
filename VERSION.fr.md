# CyAudioCore 0.2.0

EN DÉVELOPPEMENT. Sources uniquement, aucune release binaire.

CyAudioCore fournit les services audio et s'appuie sur l'API publique de CyDSP pour le calcul numérique. Les formats persistants existants sont conservés. Voir [architecture](docs/Architecture.md) et [validation](docs/Validation.md).

## Changements de la version 0.2.0

- Cette version mineure ajoute la méthode virtuelle `AudioSource::skipUnderrun(nextFrame)`, ce qui modifie l’ABI des sources. Lorsque le transport avance après un manque de données, il appelle cette méthode pour que `ReadAheadSource` puisse remplir le cache à la nouvelle position et reprendre la lecture. Les implémentations existantes peuvent garder la méthode vide fournie par défaut. Recompiler les implémentations de sources précompilées et tous les consommateurs avec les en-têtes et les archives 0.2.0. [Contrat de lecture anticipée et du transport](docs/DawIntegration.md#background-playback-and-assets)
- CyDSP 0.4.6 devient la dépendance de référence. Cette version corrige la lecture inversée des grains dans `SourceBatch` et le mixage en bypass dans `MultiBusGraph`. Recompiler Core et les applications qui l’utilisent avec le même paquet CyDSP. [Révision et instructions de compilation](docs/CyDSPCurrent.fr.md)
- Quand un plugin isolé sature son tampon de sortie MIDI, l’hôte conserve le bloc audio et signale les événements perdus. Le rejet d’une configuration de périphérique invalide laisse le flux en cours actif. À la réouverture, les proxies PCM d’édition restituent la fréquence d’échantillonnage exacte de la source, y compris les fréquences fractionnaires.
- Le chargement d’une réponse impulsionnelle réutilise le tampon décodé si le nombre de canaux correspond déjà. Lors d’un `flush` sur un plugin inactif, le worker dimensionne le tampon d’entrée selon la requête et réutilise son tampon de sortie d’un appel à l’autre.

## Changements précédents

- Compatibilité CyDSP 0.4.5 : retrait explicite du délai HRTF commun et rendu direct/FFT avec le transport et le graphe ; archives, headers et workers installés de même version. La prise en charge des paquets 0.4 installés et le contrôle exact de la version de dépendance statique sont conservés. Voir l’[intégration courante](docs/CyDSPCurrent.fr.md).
- Compatibilité avec CyDSP 0.3.2 : reconstruction des archives et contrôle du paquet installé avec sa version exacte ; couverture de la convolution via le transport et le graphe, avec blocs variables et buffers partagés. Voir la [référence courante](docs/CyDSPCurrent.fr.md).

- Les fournisseurs publics de plugins isolent chaque instance et chaque scan ; contrôles CLAP et presets par IPC, mode même cycle sans attente et worker optionnel. Voir [configuration et utilisation](docs/PluginIsolation.fr.md).

- CLAP devient le format principal : contrôles des extensions stables 1.2.10, configurations de ports, modulation polyphonique, presets, métadonnées et services applicatifs ; voir le [contrat et ses limites](docs/Clap.fr.md).

- Exige CyDSP 0.3.0. Le synthétiseur rend les enveloppes et les voix d'échantillonneur par les formes par bloc `Adsr::process` et `SampleData::render` de CyDSP, à sortie inchangée ; voir les [mesures](docs/Measurements.md).
- Sources : `ReadAheadSource::readAt` n'efface que les frames qu'il n'écrit pas ; les workers de lecture anticipée et d'enregistrement sont réveillés par une notification non bloquante au lieu d'une interrogation toutes les millisecondes.
- Fichiers : les échecs d'écriture sont signalés, le PCM entier est restitué exactement et l'export arrondit au plus proche, un fichier sans métadonnées se réexporte, une écriture par chemin ne remplace la destination qu'une fois complète, et les écarts bénins RIFF/AIFF sont tolérés dans les mêmes bornes.
- MIDI : l'entrée MIDI 1 native ne se déconnecte plus sur une lecture vide, un événement trop grand pour le tampon de destination est écarté et compté au lieu de bloquer l'entrée, MIDI-CI évince le pair vu le moins récemment, Property Exchange termine les réponses rejetées, et Flex Data Set Chord Name suit l'empaquetage standard (changement de format filaire).
- Plugins : le processus worker exécute l'audio sur son propre thread avec réveil par événement (protocole version 4), garde ses bornes hors de portée de l'enfant, se rétablit après un chargement échoué et republie la latence ; JSFX conserve l'état @serialize à l'activation et n'exécute plus @init au démarrage du transport ; un événement de sortie rejeté ne rend plus le bloc muet.
- Plugins : veille automatique CLAP/VST3 à queue finie, politiques explicites sur silence/désactivée, réveils par génération et blocs silencieux locaux ; le protocole 4 exige des helpers reconstruits ensemble.
- Tests : une entrée CTest par suite ; `cyaudio_tests <suite>...`, `--keep-going` et `--list` permettent de sélectionner et de poursuivre.

macOS et Linux desktop restent à tester. Le MIDI/audio matériel et la compatibilité avec tous les plugins ne sont pas validés. SVE reste soumis à la présence du CPU/OS. Aucun nouveau résultat ne doit être déduit de l’ancienne matrice monolithique.
JSFX utilise le backend bytecode portable ; son canvas natif est limité à Windows. AU/AAX sont des frontières de providers, pas des hôtes intégrés. L’énumération des programmes/unités VST3 reste à compléter.
