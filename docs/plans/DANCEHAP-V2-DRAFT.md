# DanceHAP v2 — Plan architecture (FINAL DRAFT)

> Statut : **Décisions validées avec Jean-Luc — 30/07/2026**
> Remplace ARCHITECTURE.md v1.0 et ROADMAP.md dès validation Gate A.

---

## 1. Vision

Plugin OBS + éditeur standalone pour transformer un setup webcam en scène
de danse immersive : danseur détouré en temps réel, clips HAP en fond et
overlay, pilotable via hotkeys/Stream Deck pendant le live.

Cas d'usage : Take That Circus / Summer 2026 LED.

---

## 2. Architecture — 2 composants

```
┌──────────────────────────────────┐       ┌──────────────────────────────┐
│  DanceHAP Studio (éditeur)       │       │  Plugin OBS DanceHAP         │
│  App standalone C++17 + Qt6      │       │  (C++17, OBS MODULE)         │
│                                  │       │                              │
│  • Timeline compositor           │ .dhp  │  • Source composite (3 DLayer)│
│  • Bibliothèque clips HAP        │──────▶│  • Capture webcam interne     │
│  • Keyframes + B-spline opacity  │       │  • Matting temps réel (7 mod) │
│  • Curve editor (handles)        │       │  • Décode HAP + crossfade     │
│  • Timeline audio                │       │  • Dock minimal (play/stop)  │
│  • Preview (rendu software)      │       │  • Hotkeys + Stream Deck      │
│  • Export .dhp                   │       │  • Lecture du show file       │
│                                  │       │                              │
│  OFFLINE — préparation            │       │  LIVE — exécution            │
└──────────────────────────────────┘       └──────────────────────────────┘
```

- **Studio** = authoring offline. Timeline compositor pour préparer le show.
- **Plugin OBS** = exécution live. Lit le show file, composite les DLayers,
  applique le matting, joue l'audio.
- **Show file .dhp** = le contrat entre les deux. JSON, versionnable.

---

## 3. Structure d'un show dans OBS

```
┌─────────────────────────────────────────────────────┐
│  OBS Scene                                          │
│                                                     │
│  [Top]   Layer 3+ : Autres overlays (natif OBS)     │
│  [Mid]   Layer 2  : DanceHAP (source composite)      │
│  [Bot]   Layer 1  : Live Video (webcam brute)        │
│                                                     │
└─────────────────────────────────────────────────────┘
```

- **Layer 1 (Live Video)** = webcam brute, filet de sécurité en bas de la stack.
- **Layer 2 (DanceHAP)** = source composite du plugin (voir §4).
- **Layer 3+ (Overlays)** = overlays natifs OBS optionnels — gérés par l'utilisateur.

---

## 4. Structure interne DanceHAP — les DLayers

DanceHAP (Layer 2) est un composite de 3 sous-layers :

```
┌─────────────────────────────────────────────────────┐
│  DanceHAP (Layer 2 OBS)                             │
│                                                     │
│  [Top]   DLayer 3 : HAP overlay timeline            │
│                    Clips HAP alpha superposés       │
│                    Crossfade entre clips            │
│                    Opacité animable (B-spline)       │
│                                                     │
│  [Mid]   DLayer 2 : LIVE (capture webcam interne)   │
│                    Webcam AVEC matting               │
│                    Danseur détouré, fond transparent │
│                    Opacité animable (B-spline)       │
│                    Matting statique (1 config/show)  │
│                                                     │
│  [Bot]   DLayer 1 : HAP background timeline          │
│                    Clips HAP de fond (décors)        │
│                    Crossfade entre clips             │
│                    Opacité animable (B-spline)       │
│                                                     │
└─────────────────────────────────────────────────────┘
```

### DLayer 1 — HAP Background
- Timeline de clips HAP (un ou plusieurs, arrangés sur la timeline)
- Crossfade configurable entre clips successifs
- Opacité animable (keyframes + interpolation B-spline)

### DLayer 2 — LIVE (capture webcam interne)
- **DanceHAP gère sa propre capture webcam** — pas de référence à une source
  OBS externe. Le plugin ouvre directement le device webcam.
- Matting appliqué : danseur détouré, fond transparent
- Opacité animable — apparition/disparition du danseur (fade in/out)
- Matting **statique** : une config (modèle + params) pour tout le show

### DLayer 3 — HAP Overlay
- Timeline de clips HAP alpha (un ou plusieurs)
- Crossfade configurable entre clips successifs
- Opacité animable (keyframes + B-spline)

---

## 5. DanceHAP Studio (éditeur)

### 5.1 Tech stack

- **C++17 + Qt6** (cross-platform Windows + macOS)
- Réutilise HapDecoder/HapDemuxer du plugin pour le preview
- Une seule stack, un seul build system (CMake)
- Qt fournit QGraphicsScene/QGraphicsView pour le timeline canvas,
  QPainter pour les courbes d'animation

### 5.2 Timeline compositor

L'éditeur est un **timeline compositor** avec :

- **Timeline linéaire avec markers** : un flux continu de 0 à fin du show.
  Des markers/cues permettent de naviguer (saut à un timecode précis).
  Hybride : linéaire pour le playback, markers pour la navigation live.

- **3 timelines de clips** (DLayer 1, DLayer 3) + config matting (DLayer 2) :
  - DLayer 1 et 3 : arranger des clips HAP, les positionner, les trimer
  - DLayer 2 : configurer le matting (dropdown 7 modèles + sliders)
  - Crossfade entre clips successifs (durée configurable)

- **Opacité animable par layer** : chaque DLayer a sa courbe d'opacité
  dans le temps. Keyframes avec interpolation B-spline.

- **Curve editor** : afficher les courbes d'animation, les modifier avec
  des handles (tangentes) à chaque keypoint. Supprimer des keyframes.
  Façon After Effects / DaVinci Resolve.

- **Timeline audio** : une ou plusieurs pistes audio (WAV/MP3/FLAC),
  placées sur la timeline, synchronisées avec les clips HAP.

- **Preview** : aperçu du composite (DLayer 1 + 2 + 3) en rendu software,
  pas besoin d'OBS. Réutilise HapDecoder.

- **Export .dhp** : sauvegarde le show file JSON (validation chemins + write).

### 5.3 UI (draft)

```
┌─────────────────────────────────────────────────────────────────────────┐
│  DanceHAP Studio                                              [Save .dhp]│
├─────────┬───────────────────────────────────────────────────────────────┤
│         │  TIMELINE                                              00:00  │
│ BIBLIO  │  ┌───────────────────────────────────────────────────────────┐│
│         │  │ DLayer 3 (overlay)  │  clip_A ████│██ clip_C ████        │││
│ clips   │  │ Opacity ──●──────●────────────●──────────●───────        ││
│ HAP     │  ├───────────────────────────────────────────────────────────┤│
│         │  │ DLayer 2 (LIVE)    │█████████████████████████████████████ ││
│ overlay │  │ Opacity ──●──────●──────●──────────────●───────          ││
│         │  │ Matting: RVM ▼  threshold: 0.5  feather: 2              ││
│ audio   │  ├───────────────────────────────────────────────────────────┤│
│         │  │ DLayer 1 (fond)    │  clip_fond_1 ████│██ clip_fond_2 ██ ││
│         │  │ Opacity ──●──────────────●──────────────────────         ││
│         │  ├───────────────────────────────────────────────────────────┤│
│         │  │ Audio 1    │  shine.wav ████████████████                ││
│         │  │ Audio 2    │           │  patience.wav ██████████████   ││
│         │  └───────────────────────────────────────────────────────────┘│
│         │  ▼ Markers:  |Patience  |Shine  |Back For Good  |Giants      │
│ [+Import│                                                               │
│         │  CURVE EDITOR (DLayer sélectionné)                            │
│         │  ┌───────────────────────────────────────────────────────────┐│
│         │  │  Opacity                                                 ││
│         │  │  100% ─●─────────●──────────────●─────────●──             ││
│         │  │   50% │      ╱╲    ╱╲            │                        ││
│         │  │    0% ───────────────────────────────────────             ││
│         │  │         00:15    00:30    00:45    01:00                 ││
│         │  └───────────────────────────────────────────────────────────┘│
├─────────┴───────────────────────────────────────────────────────────────┤
│  Preview:  [composite DLayer1+2+3, rendu software]                      │
└─────────────────────────────────────────────────────────────────────────┘
```

### 5.4 Système d'animation (keyframes + B-spline)

- **Keyframe** : point sur la timeline avec une valeur d'opacité (0.0-1.0)
- **Interpolation B-spline** : courbe lisse entre keyframes
- **Handles** : chaque keypoint a des handles (tangentes) pour ajuster la courbe
- **Édition** : cliquer-glisser les keypoints, ajuster les handles, supprimer
- **Par layer** : chaque DLayer a sa propre courbe d'opacité
- **DLayer 2 (LIVE)** : l'opacité contrôle l'apparition/disparition du danseur

---

## 6. Show File (.dhp)

Format JSON, versionnable. Décrit la composition complète.

```json
{
  "version": "2.0",
  "show": {
    "name": "Take That Circus — Summer 2026",
    "duration": 3600.0,
    "created": "2026-07-30"
  },
  "webcam": {
    "device": "default",
    "resolution": "1280x720",
    "fps": 30
  },
  "matting": {
    "model": "rvm_mobilenetv3",
    "threshold": 0.5,
    "feather": 2,
    "contour": 1.0,
    "mask_expansion": 0
  },
  "dlayers": {
    "dlayer1_background": {
      "clips": [
        {
          "id": "clip-001",
          "file": "C:/Shows/TTC2026/clips/patience_fond.mov",
          "start": 0.0,
          "duration": 180.0,
          "loop": true,
          "crossfade_out": 1.0
        },
        {
          "id": "clip-002",
          "file": "C:/Shows/TTC2026/clips/shine_fond.mov",
          "start": 179.0,
          "duration": 200.0,
          "loop": true,
          "crossfade_in": 1.0
        }
      ],
      "opacity_keyframes": [
        { "time": 0.0,   "value": 1.0, "handle_left": [0,0], "handle_right": [0,0] },
        { "time": 170.0, "value": 1.0 },
        { "time": 180.0, "value": 0.0 }
      ]
    },
    "dlayer2_live": {
      "opacity_keyframes": [
        { "time": 0.0,   "value": 0.0 },
        { "time": 5.0,   "value": 1.0 },
        { "time": 175.0, "value": 1.0 },
        { "time": 180.0, "value": 0.0 }
      ]
    },
    "dlayer3_overlay": {
      "clips": [
        {
          "id": "overlay-001",
          "file": "C:/Shows/TTC2026/overlays/patience_fx.mov",
          "start": 10.0,
          "duration": 170.0,
          "loop": true
        }
      ],
      "opacity_keyframes": [
        { "time": 0.0,   "value": 0.0 },
        { "time": 10.0,  "value": 0.8 },
        { "time": 180.0, "value": 0.0 }
      ]
    }
  },
  "audio_tracks": [
    {
      "id": "audio-001",
      "file": "C:/Shows/TTC2026/audio/patience.wav",
      "start": 0.0,
      "volume": 0.8
    },
    {
      "id": "audio-002",
      "file": "C:/Shows/TTC2026/audio/shine.wav",
      "start": 180.0,
      "volume": 0.9
    }
  ],
  "markers": [
    { "time": 0.0,   "name": "Patience" },
    { "time": 180.0, "name": "Shine" },
    { "time": 380.0, "name": "Back For Good" },
    { "time": 600.0, "name": "Giants" }
  ]
}
```

### Notes

- `webcam.device` : "default" ou nom du device. DanceHAP gère sa propre capture.
- `matting` : **statique** pour tout le show (pas de keyframes sur les params).
- `crossfade_in`/`crossfade_out` : durée en secondes du fondu entre clips.
- `opacity_keyframes` : interpolation B-spline. `handle_left`/`handle_right`
  optionnels pour B-spline non-uniforme (tangentes éditables).
- `markers` : points de navigation sur la timeline (hybride linéaire + cues).
- `dlayer2_live` : pas de `clips` — c'est du feed live. Seulement opacité.

---

## 7. Plugin OBS DanceHAP (exécution live)

### 7.1 Source composite

Le plugin expose **une seule source OBS** ("DanceHAP") qui :

1. Lit le show file `.dhp` au démarrage
2. Ouvre sa propre capture webcam interne (DLayer 2)
3. Composite les 3 DLayers en temps réel :
   - DLayer 1 : décode les clips HAP de fond, crossfade entre clips
   - DLayer 2 : capture webcam → matting → danseur détouré
   - DLayer 3 : décode les clips HAP overlay, crossfade entre clips
4. Applique l'opacité animée (B-spline des keyframes) par layer
5. Route l'audio (audio_tracks + audio embarqué HAP)

### 7.2 Dock OBS minimal

```
┌─────────────────────────────────────┐
│  DanceHAP                           │
│  Show: Take That Circus 2026        │
│  [Load .dhp...]  [Reload]           │
│                                     │
│  ⏱ 00:45 / 60:00    ▶ Play  ■ Stop  │
│                                     │
│  Markers: Patience | Shine | Back   │
│  DLayer 1: ●  DLayer 2: ●  DLayer 3:●│
└─────────────────────────────────────┘
```

- Transport : play / stop / timecode
- Load/reload show file
- Markers cliquables (saut à un timecode)
- Indicateurs visuels par DLayer (actif/inactif)
- Pas d'édition — tout se fait dans l'éditeur

### 7.3 Hotkeys (Phase 4)

- Play / Stop (transport global)
- 1 hotkey par marker (saut au timecode du marker)
- Configurable dans OBS Settings → Hotkeys
- Mapping Stream Deck via les hotkeys OBS

---

## 8. Roadmap v2

### Phase 1 — MVP vidéo ✅ (livré, v0.3.2)

`hap_clip_source` lit un .mov HAP avec vidéo + alpha + audio.

### Phase 2 — Matting ✅ (livré, v0.6.0)

`ai_matte_filter` avec 7 modèles, async worker, DirectML/CoreML/CPU.
Smoke Hephaistos en attente de validation.

### Phase 3 — Show File + Source Composite

**Objectif** : le plugin devient une source composite qui lit un show file
et gère les 3 DLayers.

**Livrables** :
- [ ] ADR-009 : Show file format (.dhp JSON)
- [ ] ADR-010 : Architecture 2-composants (éditeur + plugin)
- [ ] ADR-011 : Capture webcam interne DanceHAP
- [ ] Parser show file en C++ (nlohmann/json)
- [ ] Capture webcam interne (DLayer 2) — wrapper device webcam
- [ ] Source composite : compositing DLayer 1 + 2 + 3
- [ ] Intégration matting existant dans DLayer 2
- [ ] Opacité animable par layer (B-spline runtime)
- [ ] Crossfade entre clips HAP successifs
- [ ] Audio routing (audio_tracks du show file)
- [ ] Dock minimal Qt (load .dhp, play/stop, timecode, markers)
- [ ] Hotkeys : play/stop + 1 par marker
- [ ] Tests : parser show file, validation, compositing, crossfade

**Critère de sortie** : charger un .dhp dans OBS, voir les 3 DLayers
composites, le matting fonctionne, l'audio joue, les markers sont cliquables.

### Phase 4 — DanceHAP Studio (éditeur)

**Objectif** : app standalone pour créer et éditer des show files.

**Livrables** :
- [ ] Skeleton C++17 + Qt6 + CMake (cross-platform Win+macOS)
- [ ] Bibliothèque de clips (import, preview vignette, organiser)
- [ ] Timeline compositor (DLayer 1 + 3 timelines, DLayer 2 config)
- [ ] Système de keyframes + interpolation B-spline
- [ ] Curve editor avec handles (ajouter/déplacer/supprimer keyframes)
- [ ] Crossfade entre clips (durée configurable)
- [ ] Timeline audio (multi-pistes WAV/MP3/FLAC)
- [ ] Preview composite (rendu software via HapDecoder)
- [ ] Export .dhp (validation + JSON write)
- [ ] UI dark theme Apple minimaliste
- [ ] Markers sur la timeline (navigation)

**Critère de sortie** : créer un show file complet depuis l'éditeur, le charger
dans OBS, et tout fonctionne.

### Phase 5 — Polish + Release

**Objectif** : release publique v1.0.

**Livrables** :
- [ ] Installeur Windows (MSI) + macOS (DMG notarized)
- [ ] Signature code (Azure Windows, Developer ID macOS)
- [ ] Doc utilisateur (tutoriel éditeur + plugin + Stream Deck)
- [ ] Page de release GitHub avec binaires signés
- [ ] Tests smoke cross-OS automatisés

**Critère de sortie** : release v1.0.0 publique, installable par un non-tech.

---

## 9. ADRs

### À rédiger

| ADR | Sujet |
|-----|-------|
| ADR-009 | Show file format (.dhp JSON) |
| ADR-010 | Architecture 2-composants (éditeur + plugin) — complète ADR-002 |
| ADR-011 | Capture webcam interne DanceHAP (DLayer 2) |
| ADR-012 | B-spline interpolation pour keyframes d'opacité |
| ADR-013 | Crossfade entre clips HAP |

### À réviser

| ADR | Changement |
|-----|------------|
| ADR-002 | Architecture 4 briques → complétée par ADR-010 (source composite + éditeur) |
| ADR-004 | Stream Deck : inchangé (hotkeys OBS) |

### Inchangés

ADR-001 (plateformes), ADR-003 (matting, déjà révisé), ADR-005 (MIT),
ADR-006 (repo), ADR-007 (audio master clock), ADR-008 (pattern filtre sync).

---

## 10. Décisions actées (30/07/2026)

| # | Question | Décision |
|---|----------|----------|
| 1 | DLayer 2 / accès webcam | DanceHAP gère sa propre capture webcam interne |
| 2 | Timeline | Hybride : linéaire avec markers pour navigation |
| 3 | Matting | Statique — 1 config pour tout le show |
| 4 | Transitions entre clips | Crossfade configurable |
| 5 | Tech stack éditeur | C++17 + Qt6 |