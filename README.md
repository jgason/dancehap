# 🎬 DanceHAP

> Plugin OBS pour scènes de danse immersives : clips HAP musicaux + détourage
> webcam temps réel + overlays alpha, pilotables au bouton / hotkey / Stream Deck.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platforms: Windows + macOS](https://img.shields.io/badge/Platforms-Windows%20%2B%20macOS-blue)](#plateformes)
[![Status: Phase 0 — Cadrage](https://img.shields.io/badge/Status-Phase%200%20Cadrage-orange)](ROADMAP.md)

## ✨ Ce que ça fait

DanceHAP transforme un setup OBS en scène de danse immersive :

1. **Fond musical HAP** : clip vidéo HAP (avec alpha + audio intégré) qui joue en
   boucle ou sur déclenchement
2. **Toi, détouré live** : ta webcam est segmentée en temps réel (Robust Video
   Matting) et superposée au fond — sans fond vert, sans green screen
3. **Overlay HAP** : une couche d'effets HAP alpha (particules, lumières, logos)
   au-dessus de tout

Le tout pilotable via **bouton UI**, **raccourci clavier**, ou **Stream Deck**.

## 🎯 Cas d'usage

- **Streamer/danseur amateur** : prépare tes clips musicaux HAP, déclenche au
  bon moment, danse dessus en étant intégré visuellement
- **Perf live** : enchaîne tes morceaux avec des visuels cohérents
- **Créateur de contenu** : enregistre tes chorés avec backgrounds changeants

## 🏗️ Architecture

DanceHAP est **modulaire** : 4 briques indépendantes que tu composes dans OBS.

| Brique | Type OBS | Rôle |
|---|---|---|
| `hap_clip_source` | Source | Clip HAP vidéo + audio (fond musical) |
| `ai_matte_filter` | Filtre | Détourage temps réel webcam (RVM/MediaPipe) |
| `hap_overlay_source` | Source | Overlay HAP alpha (effets lumineux) |
| `dancehap_dock` | Dock UI | Grille de clips + boutons + settings |

Détails complets : [`ARCHITECTURE.md`](ARCHITECTURE.md)

## 📋 Statut

**Phase 0 — Cadrage** (cahier + ADRs + repo). Pas encore de code fonctionnel.

Voir [`ROADMAP.md`](ROADMAP.md) pour le phasage.

## 🔧 Stack technique

- C++17 + OBS Studio API ≥ 30
- Qt6 (UI)
- libhap + FFmpeg (décodage HAP)
- ONNX Runtime + RVM (matting ML)
- CMake + GitHub Actions CI (Windows + macOS)

## 📂 Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — cahier d'architecture complet
- [`docs/adr/`](docs/adr/) — décisions d'architecture (ADRs)
- [`ROADMAP.md`](ROADMAP.md) — roadmap par phases
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — review gates et workflow contribution
- [`docs/plans/PLAN-PHASE-1.md`](docs/plans/PLAN-PHASE-1.md) — plan Phase 1 détaillé

## 🔐 Plateformes

- ✅ Windows 10/11 (x64) — cible primaire
- ✅ macOS (Apple Silicon natif + Intel) — cible secondaire
- ⏸️ Linux — non-goal MVP

## 📜 Licence

MIT — voir [`LICENSE`](LICENSE).

Copyright © 2026 Don't Blink.

## 👥 Contribution

Projet en cadrage. Tant que la Phase 1 n'est pas livrée, les contributions
externes sont en attente. Voir [`CONTRIBUTING.md`](CONTRIBUTING.md) pour les
review gates en vigueur.

## 🙋 Auteur

Don't Blink — [github.com/dont-Blink](https://github.com/dont-Blink)
