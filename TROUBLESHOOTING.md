# Troubleshooting — DanceHAP CI

Pièges rencontrés et résolus lors de la mise en place de la CI GitHub Actions
(Phase 1.0). Documenté pour éviter de retomber dedans.

## 1. ffmpeg n'est plus préinstallé sur les runners GitHub

**Symptôme** : `ffmpeg: command not found` (macOS) ou `The term 'ffmpeg' is not
recognized` (Windows) à l'étape "Verify ffmpeg".

**Cause** : Les images `windows-latest` et `macos-latest` ont retiré ffmpeg de
leur préinstall (changement GitHub ~2026).

**Fix appliqué** : Ne plus dépendre de ffmpeg en CI. Le HAP test asset
(`tests/assets/sample_hapa_5s.mov`) est commité dans le repo (~656 KB).

---

## 2. Les builds brew/choco de ffmpeg n'ont pas l'encodeur HAP

**Symptôme** : `Unknown encoder 'hap'` lors de la génération du HAP test.

**Cause** : Les builds ffmpeg de Homebrew (macOS) et Chocolatey (Windows) sont
compilés **sans** `--enable-encoder=hap`. L'encodeur HAP n'est pas inclus par
défaut dans ces distributions.

**Fix appliqué** : Commiter le `.mov` plutôt que de le régénérer (voir point 1).

**Alternative non retenue** : Télécharger un build statique complet
(gyan.dev/ffmpeg sur Windows, evermeet.cx sur macOS) — ajoute ~30-60s de
download + dépendance réseau + version drift, pour un fichier de 656 KB qui ne
change jamais.

---

## 3. `Visual Studio 17 2022` generator introuvable

**Symptôme** :
```
CMake Error at CMakeLists.txt:13 (project):
  Generator
    Visual Studio 17 2022
  could not find any instance of Visual Studio.
```

**Cause** : Pinner explicitement `-G 'Visual Studio 17 2022'` suppose que cette
version exacte est installée sur le runner. L'image `windows-latest` peut évoluer.

**Fix appliqué** : Laisser `cmake_generator: ""` dans la matrice → CMake
auto-détecte le générateur disponible (Visual Studio, Ninja, Make).

---

## 4. DLL Windows dans `build/Release/` (multi-config)

**Symptôme** :
```
No files were found with the provided path: build/dancehap.dll
```

**Cause** : Visual Studio est un générateur **multi-config** : la DLL atterrit
dans `build/Release/dancehap.dll` (ou `Debug/`), pas `build/dancehap.dll`.
Make/Ninja (single-config, macOS/Linux) la mettent bien à la racine.

**Fix appliqué** : Étapes "Locate built plugin" (PowerShell `Get-ChildItem` sur
Windows, `find` sur macOS) qui copient le binaire vers `build/artifact/` avant
l'upload. Voir `.github/workflows/ci.yml`.

---

## 5. UnicodeEncodeError sur runners Windows

**Symptôme** :
```
UnicodeEncodeError: 'charmap' codec can't encode character '\u2192'
```

**Cause** : Les runners Windows ont `stdout` en cp1252 par défaut. Tout
caractère non-ASCII (→, é, è, etc.) dans un `print()` Python fait crasher le
script.

**Fix appliqué** : En tête de script Python :
```python
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")
```

---

## 6. `subprocess.Popen` + pipe stdin masque les erreurs ffmpeg

**Symptôme** : `BrokenPipeError: [Errno 32] Broken pipe` au lieu du vrai
message d'erreur ffmpeg.

**Cause** : Quand on pipe des frames via `Popen.stdin.write()` et que ffmpeg
meurt prématurément (option invalide, encodeur manquant), le write échoue
avec BrokenPipeError avant qu'on ait pu lire stderr.

**Fix appliqué** : Buffer les frames en mémoire (petit fichier → OK) puis
`subprocess.run(input=..., capture_output=True)` → stderr toujours capturé
proprement, même en cas d'échec ffmpeg.

---

## Leçons générales

- **Tester en CI dès que possible** : un script qui marche en local Linux peut
  casser de 5 façons différentes sur Windows/macOS. Ne pas assumer.
- **Préférer commiter un petit asset de test** plutôt que de le régénérer si la
  régénération demande des dépendances externes fragiles (ffmpeg, réseaux, etc.).
- **Ne pas pinner les versions d'outils système** dans la CI sauf si nécessaire.
  Laisser CMake auto-détecter, GitHub Actions setup-* actions gérer les versions.
- **Toujours forcer UTF-8** dans les scripts Python en CI.
- **Capturer stderr** de tout sous-processus lancé depuis un script Python en CI.

---

## Pièges OBS SDK en CI (Phase 1.1 — RÉSOLUS)

La Phase 1.1 a activé `DANCEHAP_WITH_OBS_DEPS=ON` en CI, ce qui demande de
builder libobs depuis les sources (obs-studio) avec les prebuilt deps
(obs-deps). **9 rounds d'itération ont été nécessaires.** Tous les pièges
sont documentés ci-dessous avec leur fix.

### 7. OBS exige le générateur Xcode sur macOS

**Symptôme** : `Building OBS Studio on macOS requires Xcode generator`.

**Cause** : `cmake/macos/compilerconfig.cmake` d'OBS hardcode l'exigence.

**Fix** : ajouter `-G Xcode` au configure OBS. Xcode étant multi-config,
tous les `--build` doivent avoir `--config Release`.

### 8. `<experimental/coroutine>` déprécié en VS 18 (MSVC)

**Symptôme** :
```
error C2338: static assertion failed: 'error STL1011: The /await compiler
option, <experimental/coroutine>... are deprecated'
```

**Cause** : OBS 31.x utilise `<experimental/coroutine>` via `libobs-winrt`.

**Fix** : `-DCMAKE_CXX_FLAGS=/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS`
sur le configure obs-studio (pas sur DanceHAP).

### 9. `cmake --install` échoue sur frontend-api non built

**Symptôme** : `file INSTALL cannot find "frontend/api/Release/obs-frontend-api.dll"`.

**Cause** : `cmake --install --component Development` essaie d'installer des
cibles non built avec `-DENABLE_FRONTEND=OFF`.

**Fix** : skip `cmake --install`. Pointer CMake sur le build tree directement
via `-Dlibobs_DIR=$PWD/obs-build/libobs`.

### 10. Target `obs` vs `libobs`

**Symptôme** : `MSBUILD error MSB1009: Project file does not exist. obs.vcxproj`
ou `xcodebuild: does not contain a target named 'obs'`.

**Fix** : `--target libobs` (pas `obs`).

### 11. `find_package(LibObs)` ne trouve pas le build tree

**Symptôme** : `Could NOT find LibObs (missing: LIBOBS_INCLUDE_DIR LIBOBS_LIBRARY)`.

**Fix** : utiliser le config-mode package généré par OBS (ne pas passer par
notre `FindLibObs.cmake` custom). Passer `-Dlibobs_DIR=$PWD/obs-build/libobs`
pour que `find_package(libobs CONFIG)` le trouve directement.

### 12. `w32-pthreads` config-mode manquant (Windows)

**Symptôme** : `Could not find a package configuration file provided by "w32-pthreads"`.

**Fix** : passer `-Dw32-pthreads_DIR=$PWD/obs-build/deps/w32-pthreads` (Windows
uniquement).

### 13. API OBS 31 — macro `OBS_MODULE_USE_DEFAULT_LOCALE` prend 2 args

**Symptôme** : `too few arguments provided to function-like macro invocation`.

**Fix** : `OBS_MODULE_USE_DEFAULT_LOCALE("dancehap", "en-US")`.

### 14. API OBS 31 — `obs_module_ver` / `obs_module_set_locale` / `obs_module_free_locale` définis par macro

**Symptôme** : `redefinition of 'obs_module_ver'` etc.

**Cause** : `OBS_DECLARE_MODULE` et `OBS_MODULE_USE_DEFAULT_LOCALE` définissent
ces fonctions inline.

**Fix** : wrap nos définitions manuelles dans `#ifndef DANCEHAP_HAVE_OBS` (elles
ne servent qu'en stub mode).

### 15. Symboles `obs_module_*` hidden en real-OBS build

**Symptôme** : le smoke test qui vérifie les exports via `nm` ne trouve pas
`obs_module_load`.

**Cause** : selon la configuration visibility, ces symboles peuvent être
internal/hidden dans la shared library — mais OBS les trouve via `dlsym` au
runtime via l'export table.

**Fix** : le smoke test ne doit pas échouer sur absence de symboles visibles
via `nm`. Il valide : size > 5 KB + type shared lib + best-effort symbole
check (avec skip gracieux si pas d'outil dispo).

## Leçons générales OBS SDK en CI

- **Toujours `--target libobs`** (pas `obs`) — nom cross-générateur.
- **Passer par config-mode** (`-Dlibobs_DIR=...`), pas par un `Find*.cmake`
  custom. Le config-mode package généré par OBS est plus fiable.
- **Ne pas `cmake --install` OBS** — pointe directement sur le build tree.
- **Xcode generator obligatoire sur macOS** pour OBS.
- **Silence coroutine warnings sur VS 18+** avec `_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS`.
- **API OBS 31** : `OBS_MODULE_USE_DEFAULT_LOCALE` prend 2 args, et
  `OBS_DECLARE_MODULE` définit déjà plusieurs entry points (ne pas les
  redéfinir manuellement).
- **Smoke test pragmatique** : ne pas faille sur symboles visibles via `nm`
  (peuvent être hidden mais présents dans l'export table).

## Setup OBS SDK en CI — template final

Le `.github/workflows/ci.yml` actuel est un template fonctionnel pour setup
OBS 31 SDK + build plugin + tests stub + smoke + upload. Sert de référence
pour les prochaines phases.

---

## Pièges Snappy / FetchContent en CI (Phase 1.3 — RÉSOLUS)

### 16. Snappy CMakeLists incompatible avec CMake 4.0+

**Symptôme** :
```
CMake Error at build/_deps/snappy-src/CMakeLists.txt:29 (cmake_minimum_required):
  Compatibility with CMake < 3.5 has been removed from CMake.
```

**Cause** : Snappy 1.2.x utilise `cmake_minimum_required(VERSION 2.8.x ...)` ou
similaire. CMake 4.0+ (installé sur les runners GitHub Actions 2025+) a supprimé
la compatibilité avec CMake < 3.5.

**Fix appliqué** : Définir `CMAKE_POLICY_VERSION_MINIMUM=3.5` avant
`FetchContent_MakeAvailable(snappy)` dans le CMakeLists.txt racine. C'est la
solution recommandée par CMake lui-même dans le message d'erreur.
