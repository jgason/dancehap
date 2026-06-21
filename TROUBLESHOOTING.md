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

## Pièges OBS SDK en CI (Phase 1.1 — NON RÉSOLUS)

La Phase 1.1 a essayé d'activer `DANCEHAP_WITH_OBS_DEPS=ON` en CI, ce qui
demande de builder libobs depuis les sources (obs-studio) avec les prebuilt
deps (obs-deps). **5 rounds de CI ont échoué sur 5 problèmes différents.**
Décision : pause sur ce front, on garde stub-only CI pour l'instant (valide
toute la logique du code). Build real-OBS à tester localement (voir
`BUILD-LOCAL.md`).

### 7. OBS exige le générateur Xcode sur macOS

**Symptôme** : `Building OBS Studio on macOS requires Xcode generator`.

**Cause** : `cmake/macos/compilerconfig.cmake` d'OBS hardcode l'exigence du
générateur Xcode.

**Fix partiel** : ajouter `-G Xcode` au configure OBS (mais Xcode est
multi-config, donc tous les `--build` doivent avoir `--config Release`).

**Statut** : contourné mais combine avec #10/#11.

### 8. `<experimental/coroutine>` déprécié en VS 18 (MSVC)

**Symptôme** :
```
error C2338: static assertion failed: 'error STL1011: The /await compiler
option, <experimental/coroutine>... are deprecated'
```

**Cause** : OBS 31.x utilise `<experimental/coroutine>` via `libobs-winrt`.
VS 18 Enterprise sur `windows-latest` déprécie ce header.

**Fix** : `-DCMAKE_CXX_FLAGS=/D_SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS`

### 9. `cmake --install` échoue sur frontend-api non built

**Symptôme** :
```
file INSTALL cannot find "obs-build/frontend/api/Release/obs-frontend-api.dll"
```

**Cause** : `cmake --install --component Development` essaie d'installer des
cibles qui ne sont pas build avec `-DENABLE_FRONTEND=OFF`.

**Fix tenté** : skip `cmake --install`, pointer CMake sur le build tree
directement. → a déclenché le bug #11 (find_package LibObs échoue).

### 10. Target `obs` vs `libobs`

**Symptôme** :
```
MSBUILD error MSB1009: Project file does not exist. Switch: obs.vcxproj
xcodebuild: does not contain a target named 'obs'
```

**Cause** : le target s'appelle `libobs`, pas `obs`, sur les générateurs VS et
Xcode.

**Fix** : utiliser `--target libobs` au lieu de `--target obs`.

### 11. `find_package(LibObs)` sur le build tree

**Symptôme** :
```
Could NOT find LibObs (missing: LIBOBS_INCLUDE_DIR LIBOBS_LIBRARY)
```

**Cause** : pointer `CMAKE_PREFIX_PATH` sur `obs-build/` ne suffit pas pour que
notre `cmake/FindLibObs.cmake` (qui cherche `obs.h` et `libobs`) trouve les
fichiers. Le build tree a une layout différente du install tree.

**Fix** : pas encore trouvé. Pistes à explorer plus tard :
1. Utiliser `libobsConfig.cmake` directement (`find_package(libobs CONFIG)`)
2. Faire `cmake --install` avec un composant qui existe (`--component Headers`)
3. Utiliser une action GitHub dédiée (`dennisamelig/setup-obs-studio`)
4. Copier le template CI du repo `obsproject/obs-plugintemplate`

## Recommandation pour la reprise

Quand on reprend le sujet OBS real-build en CI :
1. **Charger le skill `github-operations`** et inspecter le CI de
   `obsproject/obs-plugintemplate` — c'est le template officiel et il gère
   tout ce setup. L'inspiration directe est plus rapide qu'expérimenter.
2. Considérer l'action `dennisamelig/setup-obs-studio` ou similaire.
3. Si on n'arrive pas à faire marcher le real-OBS build en CI, accepter
   stub-only CI à long terme et tester le plugin manuellement avant chaque
   release (Gate E pré-release).
