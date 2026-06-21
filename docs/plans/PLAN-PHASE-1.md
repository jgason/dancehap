# Plan Phase 1 — `hap_clip_source`

> Statut : **Proposé, en attente Gate A (Jean-Luc).**
> Phase : MVP vidéo. Voir `ROADMAP.md`.

## Objectif

Livrer une source OBS native nommée « DanceHAP Clip » qui :
- Charge un fichier `.mov`/`.mp4` conteneur avec flux HAP/HAPA/HAPQ/HAPQ-A
- Décode la vidéo HAP (Snappy → texture GPU)
- Route l'audio via la source audio OBS (synchro A/V native)
- Expose les propriétés : chemin fichier, loop, autoplay
- Gère les états : `idle → loading → playing → looping/ended → idle`

**Critère de « done »** : on peut ajouter une source « DanceHAP Clip » dans OBS,
pointer vers un .mov HAP de test, et voir la vidéo + entendre l'audio en synchro,
sans crash, sur Windows ET macOS.

## Non-goals Phase 1

- ❌ Matting webcam (Phase 2)
- ❌ Overlay HAP (Phase 3)
- ❌ Dock UI custom (Phase 3)
- ❌ Hotkeys / Stream Deck (Phase 4)
- ❌ Optimisations perf avancées (Phase 5)

## Découpage en étapes testables

### Étape 1.0 — Skeleton build (jour 1)

**Livrables** :
- `CMakeLists.txt` racine
- Dossier `src/` avec `plugin.cpp` (entry point OBS `obs_module_load`)
- `cmake/` pour helpers (OBS deps, platform checks)
- `.github/workflows/ci.yml` — build matrix Windows + macOS (sans test pour l'instant)
- `tests/CMakeLists.txt` (squelette GoogleTest)
- Build local OK sur les deux OS

**Test** : `cmake -B build && cmake --build build` passe sans erreur.

**Risque** : setup OBS deps different Win/macOS. Mitigation : utiliser les
obs-plugintemplate de référence (https://github.com/obsproject/obs-plugintemplate).

### Étape 1.1 — Enregistrement source OBS vide

**Livrables** :
- `src/hap_clip_source.hpp` / `.cpp` avec `obs_source_info` minimal
- `obs_module_load` enregistre la source avec `obs_source_register`
- La source apparaît dans le menu « Ajouter une source » d'OBS sous le nom
  « DanceHAP Clip »
- Property view : juste un texte « chemin du fichier » (non fonctionnel)

**Test** :
- Unité : `HapClipSourceTest.CanInstantiate`
- Smoke : OBS démarre, la source est listée, on peut l'ajouter (rendu noir OK)

### Étape 1.2 — Demux container via FFmpeg

**Livrables** :
- Classe `HapDemuxer` qui ouvre un fichier, détecte les flux HAP (vidéo) + audio
- API : `open(path)`, `readNextVideoFrame()`, `readNextAudioFrame()`, `close()`
- Détection automatique du variant HAP (HAP, HAPA, HAPQ, HAPQ-A) via FourCC

**Test** :
- Unité : ouvre un .mov HAP de test (fichier court 5s), détecte bien HAPA
- Unité : ouvre un .mov avec audio, détecte flux audio + flux vidéo
- Unité : rejet propre si fichier inexistant ou corrompu

**Risque** : FFmpeg peut être bundlé avec OBS de façons différentes selon l'OS.
Mitigation : utiliser les headers FFmpeg exposés par OBS (`obs-ffmpeg-compat.h`
ou via le frontend), pas une copie séparée.

### Étape 1.3 — Décode HAP (Snappy → texture)

**Livrables** :
- Classe `HapDecoder` qui prend un paquet HAP compressé et produit une texture
- Décompression Snappy (lib vendue ou via dep)
- Upload sur texture OBS graphics : DXT1 (HAP), DXT5 (HAPA), BC7 (HAPQ/A)
- Format de sortie aligné sur ce qu'OBS attend (`gs_texture` avec bon format)

**Test** :
- Unité : decode un frame de test HAP → texture non-vide
- Unité : decode un frame HAPA → alpha channel présent
- Unité : rejet si format HAP non supporté

**Risque R6** (décodage plus lent que FFmpeg natif) : benchmark après implé,
si >2x plus lent, investiguer.

### Étape 1.4 — Boucle de rendu + audio routing

**Livrables** :
- `hap_clip_source` implémente `obs_source_info.video_tick` (avance la lecture)
- `obs_source_info.video_render` (rend la texture)
- Audio : `obs_source_info.audio_render` ou `obs_source_output_audio_data`
- Synchro A/V via horloge OBS (PTS alignés)
- État machine : `idle → loading → playing → looping → idle`

**Test** :
- Smoke OBS Windows : charge HAP test 5s, joue en boucle pendant 60s sans crash
- Smoke OBS macOS : idem
- Unité : état transitions correctes (idle→loading→playing→looping)

**Risque R2** (désynchro A/V) : si observé, ajouter buffer audio ou compensation.

### Étape 1.5 — Property view fonctionnelle

**Livrables** :
- Property « chemin fichier » via `obs_properties_add_path`
- Toggle « loop »
- Toggle « autoplay »
- Persistence via `obs_data`
- Rechargement propre si chemin modifié pendant lecture

**Test** :
- Smoke : change le chemin pendant lecture, ça switch sans crash
- Smoke : décoche loop, le clip joue une fois puis s'arrête proprement

### Étape 1.6 — CI matrix complète + tests

**Livrables** :
- `.github/workflows/ci.yml` build + tests Win + macOS sur chaque PR
- Artefact de build uploadé pour download manuel
- `tests/smoke/run_smoke_windows.ps1` et `run_smoke_macos.sh`

**Test** : PR de test verte sur les deux OS.

## Dépendances externes à figer

| Dép | Version min | Source | Licence |
|---|---|---|---|
| OBS Studio | 30.0 | https://obsproject.com | GPL v2.1 |
| Qt6 | 6.5 | via OBS build deps | LGPL v3 |
| FFmpeg/libav | celui d'OBS | via OBS | LGPL/GPL |
| libsnappy | 1.1.10 | https://github.com/google/snappy | BSD-3 |
| GoogleTest | 1.14 | https://github.com/google/googletest | BSD-3 |

## Risques Phase 1 (extrait du cahier §11)

| R | Risque | Mitigation |
|---|---|---|
| R2 | Désynchro A/V | Horloge OBS maître, PTS aligné |
| R3 | Alpha premul vs straight | Détection auto via metadata container |
| R5 | OBS API breaking | Pin OBS 30, wrappers isolés |
| R6 | Décode HAP plus lent que natif | Benchmark après implé |

## Critère de sortie Phase 1

**Démo finale** : sur une scène OBS propre, ajouter « DanceHAP Clip », pointer
vers `tests/assets/sample_hapa_5s.mov` (à produire via Jokyo), cocher loop,
cliquer play → la vidéo HAP avec alpha joue en boucle + l'audio sort, sans
crash pendant au moins 5 minutes, sur Windows ET macOS.

**Gate E (pre-release)** : applicable si on tag une v0.1.0-prealpha.

## Estimation

À discuter avec Célestin une fois le Gate A validé. Ordre de grandeur indicatif :
- 1.0 skeleton : 1-2 jours
- 1.1 enregistrement : 1 jour
- 1.2 demux : 2-3 jours
- 1.3 décodeur : 3-5 jours (le plus dense techniquement)
- 1.4 boucle rendu + audio : 2-3 jours
- 1.5 properties : 1 jour
- 1.6 CI matrix : 1-2 jours

Total indicatif : ~2 semaines de dév focus pour un Célestin solo.

## Prochaines étapes

1. **Gate A** : Jean-Luc valide ce plan → on démarre
2. Si validation, Célestin :
   - Crée la branche `feat/phase1-skeleton`
   - Implémente étape 1.0
   - Ouvre PR → Gate C → merge `main`
3. Boucle incrémentale par étape, PR par étape, tests verts à chaque merge
