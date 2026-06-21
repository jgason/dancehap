# Build local — DanceHAP

Guide pour builder et tester DanceHAP sur ta machine (macOS ou Windows).
La CI GitHub a un setup OBS SDK capricieux (voir `TROUBLESHOOTING.md` §7-11),
donc le build local est la voie rapide pour valider ton code.

## Setup rapide (stub mode, recommandé)

Le **stub mode** compile tout le code source **sans OBS installé**. Tous les
tests unitaires tournent dans ce mode. C'est ce qui valide que la logique du
plugin est correcte.

### Prérequis

- **CMake ≥ 3.22** : `brew install cmake` (macOS) ou [cmake.org/download](https://cmake.org/download) (Windows)
- **Compilateur C++17** : Xcode CLT (macOS) ou Visual Studio Build Tools (Windows)
- **Git** (déjà installé normalement)

### Commandes

```bash
git clone https://github.com/jgason/dancehap.git
cd dancehap

# Configure (stub mode par défaut — DANCEHAP_WITH_OBS_DEPS=OFF)
cmake -B build -S . -DDANCEHAP_BUILD_TESTS=ON

# Build
cmake --build build --parallel

# Run les 22 tests unitaires
ctest --test-dir build --output-on-failure
```

**Résultat attendu** : `100% tests passed, 0 tests failed out of 22`.

### Ce qui est validé par le stub mode

- ✅ Tout le code source compile (C++17, MSVC/Clang/GCC)
- ✅ `hap_clip_source` enregistre ses bonnes values (id, type, flags, defaults)
- ✅ Les properties (path/loop/autoplay) sont exposées correctement
- ✅ `obs_module_load` appelle bien `obs_register_source`
- ✅ Lifecycle create/destroy/update/activate/deactivate sans crash

### Ce qui n'est PAS validé par le stub mode

- ❌ Le plugin charge réellement dans OBS
- ❌ La source apparaît dans le menu Ajouter > Source
- ❌ Le futur décodage HAP, audio, matting — tout ça demande real OBS

## Build real-OBS (avancé, optionnel)

Pour tester que le plugin charge vraiment dans OBS, il faut les headers et la
lib libobs. Deux chemins :

### Chemin 1 — OBS Studio installé (plus simple)

1. Installe OBS Studio depuis [obsproject.com](https://obsproject.com)
2. Récupère les headers depuis le package OBS ou depuis les sources
3. Build DanceHAP avec :
   ```bash
   cmake -B build -S . -DDANCEHAP_WITH_OBS_DEPS=ON \
     -DCMAKE_PREFIX_PATH="/chemin/vers/obs-sdk"
   cmake --build build --parallel
   ```
4. Copie `build/dancehap.so` (macOS) ou `build/Release/dancehap.dll` (Windows)
   dans le dossier des plugins OBS :
   - macOS : `~/Library/Application Support/obs-studio/plugins/`
   - Windows : `C:\Program Files\obs-studio\obs-plugins\64bit\`

### Chemin 2 — Build libobs depuis les sources

Identique à ce que fait la CI (voir `.github/workflows/ci.yml` étape "Setup
OBS SDK"). Plus long, mais reproductible.

## Tests

```bash
# Tous les tests
ctest --test-dir build --output-on-failure

# Un test spécifique
ctest --test-dir build -R HapClipSourceTest --output-on-failure

# Liste les tests sans les exécuter
ctest --test-dir build -N
```

## Smoke test (vérifie les symboles de l'artifact)

```bash
./tests/smoke/check_source_registered.sh build/dancehap.so
```

Vérifie que `obs_module_load`, `obs_module_unload`, `obs_module_name` sont
bien exportés par la shared library.

## Problèmes connus

Voir `TROUBLESHOOTING.md` pour les pièges CI (stub mode local ne devrait pas
les rencontrer).
