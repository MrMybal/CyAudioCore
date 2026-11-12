# CyAudioCore 0.1.0

EN DÉVELOPPEMENT. Sources uniquement, aucune release binaire.

Séparation des responsabilités DSP et services audio ; headers, namespaces et cibles CMake distincts. Les formats persistants existants sont conservés. Voir [migration](docs/Migration.fr.md) et [validation](docs/Validation.md).

macOS et Linux desktop restent à tester. Le MIDI/audio matériel et la compatibilité avec tous les plugins ne sont pas validés. SVE reste soumis à la présence du CPU/OS. Aucun nouveau résultat ne doit être déduit de l’ancienne matrice monolithique.
JSFX utilise le backend bytecode portable ; son canvas natif est limité à Windows. AU/AAX sont des frontières de providers, pas des hôtes intégrés. L’énumération des programmes/unités VST3 reste à compléter.
