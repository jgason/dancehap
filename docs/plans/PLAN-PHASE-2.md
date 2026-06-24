# Plan Phase 2 — `ai_matte_filter` (Matting webcam temps réel)

> **Statut** : Proposé, en attente Gate A (Jean-Luc).
> **Phase** : Matting. Voir `ROADMAP.md` et `ADR-003`.

## Objectif

Livrer un filtre OBS applicable à une source webcam qui détourre la personne
en temps réel via RVM (GPU) ou MediaPipe (fallback CPU), produisant une
sortie RGBA avec alpha.

**Critère de « done »** : appliquer le filtre `ai_matte_filter` à une webcam,
danser devant, voir le fond remplacé en temps réel (alpha propre sur les
cheveux et contours), sans green screen, sur Windows ET macOS.

## Non-goals Phase 2

- ❌ Overlay HAP (Phase 3)
- ❌ Dock UI custom (Phase 3)
- ❌ Hotkeys / Stream Deck (Phase 4)
- ❌ Packaging / installer (Phase 5)

## Architecture technique (ADR-003)

```
Source webcam OBS
       │
       ▼
  ai_matte_filter (filtre OBS)
       │
       ├── RVM (ONNX Runtime)
       │     ├── CUDA EP (Nvidia Windows)
       │     ├── DirectML EP (AMD/Windows)
       │     ├── CoreML EP (macOS)
       │     └── CPU EP (fallback lent — warning UI)
       │
       └── MediaPipe Selfie Segmentation (fallback)
             └── CPU / GPU selon config
       │
       ▼
  Sortie RGBA (alpha = masque détourage)
```

## Découpage en sous-phases testables

### Étape 2.0 — Filter skeleton (enregistrement OBS)

**Livrables** :
- `src/ai_matte_filter.hpp` / `.cpp` avec `obs_source_info` (type = `OBS_SOURCE_TYPE_FILTER`, `OBS_SOURCE_TYPE_INPUT`? non, filter)
- `obs_module_load` enregistre le filtre
- Le filtre apparaît dans le menu « Ajouter un filtre » sous le nom « DanceHAP Matte »
- Le filtre est un pass-through (copie la frame entrante sans modification) en stub mode
- Capabilities vidéo : `OBS_SOURCE_CAP_VIDEO` + `OBS_SOURCE_CAP_OBSOLETE`? non, juste video.

**Tests** :
- Stub : `AiMatteFilterTest.CanInstantiate`
- Stub : le filtre pass-through ne modifie pas la frame entrante (même texture, même dimensions)
- CI : build Win+macOS passe

**Risque** : OBS filter API différente de source API. Mitigation : voir
`plugins/obs-filters/noise-filter.c` dans obs-studio source comme référence.

### Étape 2.1 — ONNX Runtime integration (link + load)

**Livrables** :
- `cmake/FindONNXRuntime.cmake` — trouver/bundler ONNX Runtime
- `#ifdef DANCEHAP_HAVE_ONNXRUNTIME` stub pattern
- `src/matte_engine.hpp` / `.cpp` — classe `MatteEngine` qui load un modèle ONNX
- Stub mode : `MatteEngine::loadModel()` retourne false (pas de modèle)
- Real mode : télécharge/bundle le modèle RVM ONNX (~25 Mo) — **pas committé dans le repo**

**Tests** :
- Stub : `MatteEngineTest.CanInstantiateWithoutModel`
- Stub : `MatteEngine::infer()` retourne un masque vide en stub
- CI : build passe sans ONNX Runtime installé (stub)

**Risque R-ONNX-1** : ONNX Runtime n'est pas un package vcpkg/brew standard.
Mitigation : télécharger le package officiel (nuget Windows, tar.gz macOS) en
CI, et documenter l'install locale pour dév.

### Étape 2.2 — RVM inference (CPU provider first)

**Livrables** :
- Download script pour le modèle RVM ONNX (`scripts/download_rvm_model.sh`)
- `MatteEngine::infer()` real impl : preprocess (resize 1080p → 512px), run, postprocess (masque → alpha)
- CPU execution provider d'abord (portable, testable)
- Properties UI minimale : toggle « Enable matte »

**Tests** :
- Stub : preprocess/postprocess functions (dimension, format)
- Real : `MatteEngine::infer()` sur une image de test → masque non-vide (CI avec ONNX Runtime)

**Risque R-RVM-1** : RVM attend 4 frames de context (recurrent). Mitigation :
utiliser le mode `static` (single frame) d'abord, puis ajouter le recurrent
mode en 2.2.b si perf le permet.

### Étape 2.3 — GPU providers (CUDA / DirectML / CoreML)

**Livrables** :
- Détection automatique du provider disponible (CUDA > DirectML > CoreML > CPU)
- Properties UI : choix provider (Auto / Forcer CPU)
- Warning UI si CPU seul (« Mode dégradé — utilisez un GPU pour 60fps »)
- Benchmark FPS dans le log OBS au démarrage

**Tests** :
- Stub : provider selection logic (enum, fallback chain)
- CI : build avec provider conditionnel (ne link CUDA sur macOS, etc.)

**Risque R-GPU-1** : CUDA toolkit en CI = lourd. Mitigation : CI build CPU-only,
smoke test GPU sur Hephaistos (Jean-Luc).

### Étape 2.4 — MediaPipe fallback

**Livrables** :
- `src/matte_engine_mediapipe.cpp` — intégration MediaPipe Selfie Segmentation
- Modèle TFLite (~5 Mo) bundled ou téléchargé
- Properties UI : choix modèle (Auto / RVM / MediaPipe)
- Auto-switch si RVM < 20 fps → MediaPipe

**Tests** :
- Stub : modèle selection logic
- Real : MediaPipe infer sur image test → masque non-vide

**Risque R-MP-1** : MediaPipe C++ API complexe (Bazel, abseil, protobuf).
Mitigation : utiliser `mediapipe/tasks/cc/vision/segmenter` ou le TFLite direct
avec le modèle Selfie Segmentation (~plus simple).

### Étape 2.5 — Properties UI complète

**Livrables** :
- `obs_properties_add_list` modèle (Auto / RVM / MediaPipe)
- `obs_properties_add_list` qualité (Perf / Équilibré / Qualité — change résolution interne)
- `obs_properties_add_int_slider` seuil edge (softness 0-100)
- `obs_properties_add_bool` spill suppression
- `obs_properties_add_bool` temporal smoothing
- Persistence via `obs_data`

**Tests** :
- Stub : property count, defaults, range validation
- Smoke OBS : UI visible, changements appliqués en temps réel

### Étape 2.6 — Temporal smoothing (anti-flicker)

**Livrables** :
- Buffer N dernières frames de masques
- Blending temporel (EMA ou médian)
- Toggle on/off via property

**Tests** :
- Stub : buffer logic, blending math
- Smoke : flicker visible sans smoothing vs sans avec

### Étape 2.7 — Performance benchmarks

**Livrables** :
- Script bench : FPS par config type (CUDA, DirectML, CPU, MediaPipe)
- Log OBS : `blog(LOG_INFO, "[DanceHAP] matte: %s provider, %.1f fps")`
- Warning UI si < 30 fps

**Tests** :
- Bench sur Hephaistos (Jean-Luc) — pas automatisable en CI

## Dépendances externes

| Dép | Version | Source | Licence | Taille |
|---|---|---|---|---|
| ONNX Runtime | 1.17+ | https://github.com/microsoft/onnxruntime | MIT | ~50 Mo (avec EPs) |
| RVM model ONNX | rvm_mobilenetv3 | https://github.com/PeterL1n/RobustVideoMatting | MIT | ~25 Mo |
| MediaPipe TFLite | selfie_segmentation.tflite | https://storage.googleapis.com/mediapipe-assets/ | Apache 2.0 | ~5 Mo |
| CUDA toolkit | 12+ | https://developer.nvidia.com/cuda | proprietary | (optionnel) |

## Risques Phase 2

| R | Risque | Mitigation |
|---|---|---|
| R-ONNX-1 | ONNX Runtime packaging complexe | NuGet Windows, tar.gz macOS, CI télécharge |
| R-RVM-1 | RVM recurrent mode (4 frames context) | Static mode d'abord, recurrent en option |
| R-GPU-1 | CUDA toolkit lourd en CI | CI = CPU-only, GPU smoke sur Hephaistos |
| R-MP-1 | MediaPipe C++ build complexe | TFLite direct (plus simple que Bazel) |
| R-PERF-1 | RVM CPU < 10 fps | Warning UI + auto-switch MediaPipe |
| R-ALPHA-1 | Alpha premul vs straight | Détection auto via metadata source OBS |

## Critère de sortie Phase 2

**Démo finale** : sur une scène OBS avec une webcam, ajouter le filtre
« DanceHAP Matte », danser devant → le fond est remplacé en temps réel
(alpha propre), à 30+ fps sur GPU, sur Windows ET macOS.

**Gate E** : applicable si on tag une v0.4.0-beta.

## Estimation

- 2.0 skeleton : 1 jour
- 2.1 ONNX link : 1-2 jours (setup CMake)
- 2.2 RVM CPU : 2-3 jours (preprocess/infer/postprocess)
- 2.3 GPU providers : 2-3 jours (CUDA + DirectML + CoreML)
- 2.4 MediaPipe : 2-3 jours
- 2.5 UI : 1 jour
- 2.6 smoothing : 1 jour
- 2.7 bench : 0.5 jour

Total indicatif : ~2 semaines de dév focus.

## Prochaines étapes

1. **Gate A** : Jean-Luc valide ce plan
2. Si validation, Célestin démarre 2.0 (filter skeleton)
3. Boucle incrémentale par sous-phase, PR par sous-phase, tests verts