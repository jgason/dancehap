# DanceHAP — Cahier d'architecture v1.0

> Statut : **Validé (Gate A) le 2026-06-21 par Jean-Luc Gason.**
> Toute modification substantielle doit passer par un nouvel ADR ou une PR de revue.

## 1. Vision

Un plugin OBS qui transforme un setup webcam en **scène de danse immersive** :
le danseur est détouré en temps réel et placé devant un clip HAP musical (fond),
avec possibilité d'ajouter un overlay HAP alpha par-dessus. Déclenchement
instantané via bouton UI, raccourci clavier configurable, ou Stream Deck.

Cas d'usage principal : Jean-Luc (et tout streamer/danseur amateur) veut pouvoir
lancer un clip musical HAP, danser dessus en étant détouré live, et garder une
couche d'effets lumineux HAP par-dessus — le tout pilotable au doigt et à l'œil
pendant un live ou un enregistrement.

## 2. Périmètre

### In-scope MVP
- 3 layers composites : HAP-fond (vidéo + audio) / webcam détourée / HAP-overlay alpha
- Déclenchement clip : bouton UI, hotkey OBS configurable, Stream Deck (via hotkeys)
- Sélection webcam + modèle de matting temps réel (RVM GPU, fallback MediaPipe)
- UI dock OBS épurée, orientée grille de clips

### Non-goals MVP
- Capture écran / fenêtre comme source — OBS le fait déjà nativement
- Édition / génération de HAP dans le plugin
- Multi-danseurs / multi-webcams simultanés
- Cloud streaming distant / rendu serveur
- Plugins Elgato natifs (reporté Phase 5)

## 3. Architecture modulaire

### 3.1 Principe

Quatre briques indépendantes, exposées chacune comme un objet OBS natif
(source, filtre, ou dock). L'utilisateur compose sa scène via le pipeline OBS
standard. Chaque brique est testable isolément et réutilisable ailleurs.

```
┌─────────────────────────────────────────────────────────┐
│  DanceHAP Dock (UI Qt)                                  │
│  • grille de clips HAP (drag & drop, vignettes)         │
│  • boutons déclenchement                                 │
│  • binding hotkeys + config webcam/matte                │
└──────────────────┬──────────────────────────────────────┘
                   │ orchestre via API interne
       ┌───────────┼───────────────┬───────────────┐
       ▼           ▼               ▼               ▼
  [Source OBS] [Filtre OBS]   [Source OBS]    [Hotkey mgr]
  HAP-Clip     AI-Matte        HAP-Overlay     (OBS native)
  + audio      (RVM/MediaPipe) alpha-only
```

### 3.2 Rationale modulaire vs mono-source

- Chaque brique testable sola, sans dépendances croisées
- L'utilisateur garde le contrôle total du compositing OBS
- Évite de réinventer le mixer A/V, l'audio monitoring, les transitions — OBS gère
- Permet d'activer/désactiver une brique sans casser le reste (ex: matte off si GPU faible)

## 4. Briques — spécification détaillée

### 4.1 `hap_clip_source` — source OBS (vidéo + audio)

**Responsabilité** : lire un fichier HAP (MOV/MP4 conteneur), décoder la piste
vidéo HAP et la piste audio, exposer les deux comme une source OBS.

**Pipeline interne** :
1. Demux container (FFmpeg/libav — déjà dans OBS) → flux vidéo HAP + flux audio
2. Décode HAP : décompression Snappy → upload texture DXT1/5/BC7 sur GPU OBS
3. Audio : routing via l'audio source OBS natif (synchro A/V maître OBS)
4. États machine : `idle → loading → playing → looping/ended → idle`

**API interne (C++)** :
- `load(path)` — charge et prépare
- `play()`, `stop()`, `pause()`
- `seek(timestamp_ms)`
- `set_loop(bool)`
- `get_duration()` / `get_position()`

**Gestion alpha** : détection automatique du format HAP (HAP, HAPA, HAPQ, HAPQ-A).
Toggle UI pour interprétation premultiplied vs straight (cf. ADR sur le sujet
si divergences détectées).

### 4.2 `ai_matte_filter` — filtre OBS (sur source webcam)

**Responsabilité** : appliquer un détourage temps réel sur une source vidéo
(webcam ou autre) et produire une sortie RGBA avec alpha.

**Modèle par défaut** : **Robust Video Matting (RVM)** via ONNX Runtime.
- Execution providers : CUDA (Nvidia), DirectML (AMD/Windows), CoreML (macOS),
  CPU (fallback lent — warning UI).
- Modèle bundlé léger (~25 Mo) téléchargé à l'installation.

**Fallback léger** : MediaPipe Selfie Segmentation (Google) — CPU friendly,
qualité moindre mais tourne partout.

**Options UI** :
- Choix modèle (Auto / RVM / MediaPipe)
- Niveau qualité (Perf / Équilibré / Qualité)
- Seuil edge (softness)
- Spill suppression (réflexion verte/fond)
- Toggle on/off

**Contrainte** : GPU dédié fortement recommandé. Avertissement UI explicite si
CPU seul.

### 4.3 `hap_overlay_source` — source OBS (alpha-only)

**Responsabilité** : variante de 4.1 sans piste audio. Sert pour particules,
logos animés, effets lumineux. Mixage alpha standard OBS (blend mode
configurable : normal / additive / screen).

### 4.4 `dancehap_dock` — dock UI Qt

**Vue principale** : grille de clips HAP chargés, chaque cellule = vignette +
nom + durée + bouton ▶. Drag & drop .mov/.mp4 pour ajouter. Clic ▶ déclenche,
re-clic stoppe.

**Panel settings (repliable)** :
- Sélection webcam (dropdown sources OBS)
- Sélection modèle matte + qualité
- Édition hotkeys (renvoi vers OBS hotkey settings)
- Toggle overlay

**Indicateurs** :
- Clip en lecture : vignette animée + bordure accent
- Barre de progression discrète
- État matting (GPU/CPU, FPS estimé)

## 5. UX / UI — principes directeurs

1. **One-glance** : tout l'usage courant tient dans la grille + bouton play
2. **Pas de jargon technique** : « Caméra », « Fond », « Effet » — jamais
   « Source 1 », « Filter 2 »
3. **Defaults intelligents** : webcam auto-détectée, matting « équilibré »
4. **Feedback visuel constant** : pas de popup, pas de modal pour les actions
   courantes
5. **Thème sombre aligné sur OBS**, accents couleur à définir (cf. ADR-007
   futur sur le design system)

## 6. Stack technique

| Brique | Techno |
|---|---|
| Plugin OBS | C++17, OBS Studio API ≥ 30 |
| UI | Qt6 (livré avec OBS) |
| Décode HAP | libhap (open source) + Snappy + texture upload OBS graphics |
| Demux/audio | FFmpeg/libav (déjà présent dans OBS) |
| Matting ML | ONNX Runtime + RVM .onnx (+ MediaPipe fallback) |
| Build | CMake ≥ 3.22, CI GitHub Actions (Windows + macOS) |
| Tests | GoogleTest (C++), smoke test OBS headless |

## 7. Plateformes cibles

- **Windows 10/11** (x64) — cible primaire (90% du marché streaming)
- **macOS** (Apple Silicon natif + Intel) — cible secondaire
- **Linux** : non-goal MVP, possible plus tard

Voir ADR-001 pour le détail.

## 8. Déclencheurs

Trois chemins supportés au MVP :

1. **Bouton UI** dans le dock (clic souris)
2. **Hotkey OBS** (configurable via `obs_hotkey_register`) — 1 hotkey par clip
   + 1 hotkey « stop all »
3. **Stream Deck** : au MVP, mappé via l'app officielle Elgato sur les hotkeys
   OBS (zéro code Elgato). Plugin Elgato natif reporté Phase 5 (cf. ADR-004).

## 9. Sécurité & données

- **Aucune donnée envoyée à l'extérieur** : matting 100% local
- **Pas de télémétrie** par défaut
- **Webcam** : permissions OS gérées par OBS, le plugin n'accède jamais directement
  au périphérique
- **HAP clips** : lus depuis disque local, jamais uploadés
- **Modèles ML** : bundlés/téléchargés depuis le release GitHub du projet

## 10. Performance — cibles

| Config | FPS visé | Résolution |
|---|---|---|
| GPU dédié (Nvidia RTX / AMD RX / Apple Silicon) | 60 fps | 1080p |
| GPU intégré récent (Iris Xe, RDNA iGPU) | 30 fps | 720p |
| CPU seul | 15 fps | 480p (warning UI) |

Budget : latence matting < 33 ms (1 frame @30 fps) pour éviter les artefacts
de bordure. Temporal smoothing activé par défaut.

## 11. Risques identifiés

| ID | Risque | Probabilité | Impact | Mitigation |
|---|---|---|---|---|
| R1 | Latence matting > 1 frame → artefacts bordure | Moy | Haut | Modèle léger + temporal smoothing + réglage seuil |
| R2 | Désynchro A/V HAP (audio en avance/retard) | Moy | Haut | Horloge OBS maître, PTS audio aligné sur vidéo |
| R3 | Alpha premultiplied vs straight (HAP supporte les deux) | Moy | Moy | Détection auto + toggle UI explicite |
| R4 | Providers matting cross-platform inégaux | Élevé | Moy | Build matrix CI + fallback MediaPipe + CPU |
| R5 | OBS API breaking change entre versions | Faible | Haut | Pin version min OBS 30, wrappers isolés |
| R6 | Décodage HAP plus lent que FFmpeg standard | Faible | Moy | Benchmark vs decode HAP natif OBS s'il existe |
| R7 | Taille binaire (ONNX + RVM + deps) > 100 Mo | Moy | Faible | Modèle téléchargé post-install, stripping binaire |

## 12. Roadmap (résumé)

Voir `ROADMAP.md` pour le détail par phase. Phasage :

- **Phase 0** (cadrage) : cahier + ADRs + repo + skeleton build — *ce document*
- **Phase 1** (MVP vidéo) : `hap_clip_source` lecture HAP + alpha + audio
- **Phase 2** (matting) : `ai_matte_filter` RVM GPU + fallback
- **Phase 3** (overlay + dock) : `hap_overlay_source` + dock UI + drag&drop
- **Phase 4** (déclencheurs) : hotkeys + boutons + mapping Stream Deck
- **Phase 5** (polish) : presets, perf, packaging installeur, plugin Elgato natif, doc utilisateur

## 13. Review gates

Voir `CONTRIBUTING.md` pour le détail. Résumé :

- **Gate A — Plan review** : plan relu avant code
- **Gate B — Design review** : pour changements >1 module ou impact §5/§6/§7
- **Gate C — Code review** : toute PR, reviewer ≠ auteur
- **Gate D — Security review** : features sensibles (ici : webcam, matting local)
- **Gate E — Pre-release review** : tests verts, CHANGELOG, plan rollback, smoke prod
- **Gate F — Post-mortem** : template standard pour incidents

## 14. Références externes

- OBS Studio plugin API : https://obsproject.com/docs
- HAP codec spec : https://hap.video/
- libhap : https://github.com/Vidvox/hap
- Robust Video Matting : https://github.com/PeterL1n/RobustVideoMatting
- MediaPipe Selfie Segmentation : https://google.github.io/mediapipe/
- ONNX Runtime : https://onnxruntime.ai/
- Jokyo HAP Encoder (référence encodeur) : voir skill `jokyo-hap-encoding`
