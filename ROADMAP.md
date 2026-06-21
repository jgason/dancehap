# Roadmap — DanceHAP

Dernière mise à jour : 2026-06-21

## Phase 0 — Cadrage ✅

**Objectif** : établir le cahier d'architecture, les ADRs, le repo, et valider
le plan d'implémentation Phase 1.

**Livrables** :
- [x] Cahier d'architecture (`ARCHITECTURE.md`) — validé par Jean-Luc
- [x] ADRs 001 à 006 — validés
- [x] Repo `dont-Blink/dancehap` créé
- [x] CONTRIBUTING.md (review gates)
- [x] PLAN-PHASE-1.md rédigé
- [x] Gate A validé pour Phase 1 → déclencheur de code

**Critère de sortie** : Jean-Luc valide le plan Phase 1, Célestin démarre le code.

---

## Phase 1 — MVP vidéo (`hap_clip_source`)

**Objectif** : une source OBS qui lit un fichier HAP (avec alpha + audio) et
l'affiche correctement dans une scène. Pas de matting, pas d'overlay, pas d'UI
au-delà de la property view OBS standard.

**Livrables** :
- [x] Skeleton CMake + build Windows + macOS (Phase 1.0, PR #1)
- [x] Wrapper source OBS `hap_clip_source` enregistré (Phase 1.1, PR #2)
- [x] Property view OBS : chemin du fichier, toggle loop, toggle autoplay (Phase 1.1)
- [x] Tests unitaires GoogleTest (62 tests, stub mode) (Phase 1.0–1.3)
- [x] CI GitHub Actions matrix Win+macOS avec real-OBS build (Phase 1.1)
- [x] Demux container (FFmpeg/libav) → extraction flux HAP + audio (Phase 1.2, PR #3)
- [x] Décode HAP : Snappy decompress → upload texture DXT1/DXT5 (Phase 1.3, PR #4)
- [ ] Routing audio OBS natif (infra en place Phase 1.4, décodage AAC = Phase 1.5+)
- [x] États : idle / loading / playing / looping / ended (Phase 1.4 ClipPlayer, PR #5)
- [ ] Smoke test OBS Windows (SMOKE-1.4.md rédigé, à exécuter post-merge)

**Critère de sortie** : on peut ajouter une source « DanceHAP Clip » dans OBS,
pointer vers un .mov HAP, et voir la vidéo + entendre l'audio en synchro.

---

## Phase 2 — Matting (`ai_matte_filter`)

**Objectif** : un filtre OBS applicable à une source webcam qui détourre la
personne en temps réel via RVM (GPU) ou MediaPipe (fallback).

**Livrables** :
- [ ] Wrapper filtre OBS `ai_matte_filter`
- [ ] Intégration ONNX Runtime + providers CUDA/DirectML/CoreML/CPU
- [ ] Modèle RVM bundlé + downloader
- [ ] Fallback MediaPipe Selfie Segmentation
- [ ] UI filtre : modèle, qualité, seuil edge, spill suppression, toggle
- [ ] Temporal smoothing (anti-flicker)
- [ ] Warning UI si CPU seul
- [ ] Tests perf (bench FPS par config type)

**Critère de sortie** : appliquer le filtre à une webcam, danser devant, voir
le fond remplacé en temps réel sans green screen.

---

## Phase 3 — Overlay + Dock UI

**Objectif** : la 3ème couche HAP alpha + le dock UI épuré pour gérer les clips.

**Livrables** :
- [ ] Wrapper source OBS `hap_overlay_source` (alpha-only, sans audio)
- [ ] Dock Qt `dancehap_dock`
- [ ] Grille de clips avec vignettes + drag & drop .mov/.mp4
- [ ] Bouton ▶ par clip (toggle play/stop)
- [ ] Barre de progression discrète
- [ ] Panel settings repliable (webcam, modèle matte)
- [ ] Wizard de premier démarrage (ajoute les 3 sources + filtre auto)
- [ ] Thème sombre aligné OBS

**Critère de sortie** : setup complet via le dock uniquement, sans toucher au
panneau sources OBS standard.

---

## Phase 4 — Déclencheurs

**Objectif** : piloter les clips via hotkeys et Stream Deck (mapping hotkey).

**Livrables** :
- [ ] Enregistrement hotkey OBS par clip (1 hotkey/clip + 1 « stop all »)
- [ ] UI d'affection hotkey dans le dock
- [ ] Doc utilisateur « configurer son Stream Deck »
- [ ] Tests d'intégration hotkey

**Critère de sortie** : on peut lancer un clip en appuyant sur un bouton
Stream Deck physique, sans toucher la souris.

---

## Phase 5 — Polish

**Objectif** : rendre DanceHAP prêt pour une release publique v1.0.

**Livrables** :
- [ ] Presets (sauvegarde configuration de scène)
- [ ] Optimisations perf (profilage, cache, async)
- [ ] Packaging : installeur Windows (MSI/NSIS), DMG notarized macOS
- [ ] Plugin Elgato natif (cf. ADR-004) — conditionnel au retour MVP
- [ ] Doc utilisateur complète (screenshots, tutoriels vidéo)
- [ ] Page de release GitHub avec binaires signés
- [ ] Smoke tests cross-OS automatisés en CI

**Critère de sortie** : release v1.0.0 publique, téléchargeable, installable
par un non-technicien.

---

## Backlog (post-1.0, non daté)

- Support Linux
- Multi-danseurs / multi-webcams
- Capture écran / fenêtre comme source pour le matting
- Éditeur HAP intégré (trim, loop points)
- Effets additionnels (chroma key, glow, shadow)
- Thèmes UI personnalisables
- Export de scène (template partageable)
- Marketplace de clips HAP
