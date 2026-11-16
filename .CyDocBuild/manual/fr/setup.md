---
id: setup
title: Compiler et lier
description: Installer les bibliothèques statiques et rendre un premier bloc.
section: Guides
order: 1
sourceRevision: 2
parent: overview
---

## Prérequis {#requirements}

Utilisez CMake 3.24+, un compilateur C++20 et CyDSP 0.3.x ou 0.4.x (minimum 0.3.0). La dépendance courante est 0.4.0. Obtenez CyDSP séparément. Gardez des headers, archives, runtimes C++ et architectures cohérents entre application et workers. Recompilez les consommateurs quand une dépendance change la disposition de ses objets C++. Le paquet installé de Core demande la version exacte de CyDSP utilisée pour compiler ses archives statiques.

## Compiler les sources {#build}

Depuis le dossier CyAudioCore, avec un checkout CyDSP voisin sous Windows x64 :

```sh
cmake -S . -B build/release -G "Visual Studio 17 2022" -A x64 -DCYAUDIOCORE_CYDSP_SOURCE_DIR=../CyDSP -DCYAUDIOCORE_BUILD_PLUGIN_WORKER=ON
cmake --build build/release --config Release
cmake --install build/release --config Release --prefix build/install
```

`CYAUDIOCORE_CYDSP_SOURCE_DIR` consomme explicitement ce checkout. Sans ce chemin, CMake cherche un paquet CyDSP installé ; le projet parent peut aussi fournir une cible `CyDSP::CyDSP` existante. La configuration ne télécharge rien. Les bibliothèques de production sont statiques ; tests et mesures restent optionnels. Les backends système demandent leurs SDK natifs.

`CYAUDIOCORE_BUILD_PLUGIN_WORKER=ON` construit et installe `cyaudio_plugin_worker` sans l’exécuter. Activez `CYAUDIOCORE_WITH_PORTAUDIO=ON` pour ajouter ce [fournisseur audio](doc:audio#providers). `Sources` exige `Files` et utilise la cible de stretching optionnelle de CyDSP quand elle est disponible.

## Lier une application {#link}

Avec les paquets installés dans un préfixe commun, placez ceci à côté de `main.cpp` :

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyAudioApp LANGUAGES CXX)
find_package(CyAudioCore 0.1 CONFIG REQUIRED)
add_executable(MyAudioApp main.cpp)
target_link_libraries(MyAudioApp PRIVATE CyAudioCore::CyAudioCore)
```

Renseignez `CMAKE_PREFIX_PATH` avec le préfixe d’installation à la configuration de votre application. La cible d’agrégation lie les modules présents dans ce paquet. Demandez les modules nécessaires par `find_package(... COMPONENTS Core Files Devices Sources Plugins)` ou liez leurs [cibles individuelles](doc:api#modules).

## Rendre en mémoire {#first-block}

Ce `main.cpp` complet n’ouvre ni périphérique ni fichier. La préparation alloue le stockage ; le transport lit une source en mémoire empruntée.

```cpp
#include <cyaudio/sources/AudioTransport.h>

int main() {
    using namespace cyaudio;
    AudioBuffer<float> samples, output;
    if (samples.resize(2, 128) != Status::ok ||
        output.resize(2, 64) != Status::ok) return 1;
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (auto& sample : samples.block().channel(ch)) sample = .25f;
    MemoryAudioSource<float> source;
    AudioTransport<float> transport;
    if (source.prepare(samples.block(), 48000) != Status::ok ||
        transport.prepare({48000, 64, 2}, source) != Status::ok ||
        transport.enqueue({TransportAction::play}) != Status::ok ||
        transport.process(output.block()) != Status::ok) return 2;
    return output.block().channel(0)[0] == .25f ? 0 : 3;
}
```

La source doit vivre plus longtemps que le transport et ses échantillons doivent rester immuables. Dans un callback, réutilisez la sortie préparée et vérifiez chaque statut retourné. Continuez avec les [périphériques audio](doc:audio) ou la [lecture de fichiers](doc:sources).

## Assets HRTF et SOFA optionnels {#hrtf}

Avec CyDSP 0.4, préparez les banques HRTF, HRIR analytiques et décodeurs HOA par son API publique, puis utilisez leurs appels `process` dans vos callbacks de graphe. La préparation appartient au thread de contrôle. Transmettez au graphe la latence et la traîne annoncées par le processeur ; la conception HOA spectrale et les politiques de délai fractionnaire peuvent ajouter de la latence. Core ne choisit pas de profil de qualité spatiale et n’applique pas automatiquement de rendu.

Les assets HRTF décodés/cache et les lecteurs SOFA sont des cibles CyDSP séparées et optionnelles. Pour une compilation depuis les sources, `CYDSP_BUILD_HRTF_ASSETS=ON` active `CyDSP::HrtfAssets` ; `CYDSP_BUILD_SOFA=ON` active aussi `CyDSP::Sofa`. Les deux sont désactivés par défaut. Le lecteur compact se sélectionne par `CYDSP_SOFA_COMPACT` ; le lecteur complet exige l’option explicite `CYDSP_SOFA_COMPLETE` et les SDK externes HDF5/ZLIB. Obtenez les jeux de données séparément et conservez leurs licences ainsi que les notices distribuées avec CyDSP.

Pour un Core installé, compilé contre CyDSP 0.4 avec SOFA activé :

```cmake
find_package(CyAudioCore 0.1 CONFIG REQUIRED)
find_package(CyDSP ${CyAudioCore_CYDSP_VERSION} EXACT CONFIG REQUIRED COMPONENTS Sofa)
target_link_libraries(MyAudioApp PRIVATE CyAudioCore::CyAudioCore CyDSP::Sofa)
```

L’agrégat de Core ne lie pas le lecteur. L’hôte importe explicitement depuis la mémoire et prépare les données avant le traitement ; l’accès aux fichiers, la propriété et le choix du jeu de données restent sa responsabilité.
