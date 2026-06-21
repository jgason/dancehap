# Plan Phase 1.4 — Boucle de rendu + synchro A/V

> Statut : **Proposé, en attente Gate A (Jean-Luc).**
> Parent : `PLAN-PHASE-1.md` étape 1.4.
> Prérequis livrés : 1.0 ✅, 1.1 ✅, 1.2 ✅, 1.3 ✅ (main `f9cf4d3`).

## Objectif

Brancher les briques 1.2 (demux) et 1.3 (decode) sur les hooks de timing OBS
pour rendre DanceHAP **visible et audible** dans une scène. À la fin de cette
phase, on peut ajouter une source « DanceHAP Clip », pointer vers
`tests/assets/sample_hapa_5s.mov`, et voir la vidéo + entendre l'audio en
synchro, en boucle, sans crash — sur Windows ET macOS.

## Critère de « done »

1. La source « DanceHAP Clip » produit une vidéo non-noire dans la préview OBS
   quand on lui donne un `.mov` HAP valide.
2. L'audio du clip sort par le mixer OBS (sur la piste « DanceHAP Clip »).
3. Synchro A/V : pas de drift perceptible > 80 ms après 60 s de lecture.
4. Loop fonctionne : le clip rejoue indéfiniment sans stutter ni fuite mémoire.
5. `loop=false` : le clip joue une fois puis passe en état `Ended` (frame fixée
   sur la dernière image, pas de crash).
6. Smoke test 60s sans crash sur Windows + macOS.
7. Tests unitaires couvrent la machine à états + la logique de pacing en stub
   mode (sans OBS / FFmpeg / Snappy installés).

## Non-goals 1.4

- ❌ Property view avancée (Phase 1.5)
- ❌ Seek manuel / scrubbing (backlog)
- ❌ Volume control / mute depuis la source (OBS le gère via le mixer)
- ❌ Optimisation perf (Phase 5) — on accepte un decode qui peut stutter sur
  très haute résolution, l'objectif est la *justesse* d'abord.

## Risques spécifiques 1.4

| R | Risque | Mitigation |
|---|--------|-----------|
| R2 | Désynchro A/V > 80 ms | Audio master clock quand audio présent (ADR-007), vidéo esclave des PTS |
| R7 | Stutter au démarrage (premières frames lentes à décoder) | Pré-roll : décoder 3 frames avant `Playing`, état `Loading` meanwhile |
| R8 | Fuite mémoire sur loop (texture recréée à chaque frame) | Vérifier `gs_texture_destroy` avant `gs_texture_create` si dimensions identiques ; sinon `gs_texture_set_image` sur texture existante |
| R9 | Thread-safety : `video_tick` (vidéo thread) vs `audio_render` (audio thread) partagent `demuxer` | Mutex sur le demuxer OU lecture démuxée centralisée dans `video_tick` + queue audio lock-free |
| R10 | `video_tick` appelé à ~60 Hz mais clip à 30 fps | Pacing : ne pas décoder si `elapsed < next_frame_pts` |

## Décision d'architecture : ADR-007 (à valider)

**Audio master clock** : quand le clip a un flux audio, l'audio pilote
l'horloge. Les frames vidéo sont décodées pour aligner leur PTS sur l'horloge
audio (pull model piloté par les timestamps poussés via
`obs_source_output_audio_data`).

Si pas d'audio : fallback sur **system clock** (`os_gettime_ns()`),
video master.

Rationale : OBS mixer audio tourne à sample-rate fixe (48 kHz), c'est la
référence temporelle la plus stable. Synchroniser la vidéo dessus évite le
drift long-terme. Ce pattern est utilisé par les sources media natives d'OBS
(`obsffmpeg/obs-ffmpeg-media.c`).

## Découpage en étapes testables

Vu la complexité, **une seule PR** mais avec commits atomiques par sous-étape.
Célestin peut pousser sa branche entre sous-étapes pour feedback intermédiaire.

### 1.4.1 — Classe `ClipPlayer` (state machine)

**Livrables** :
- Nouvelle classe `dancehap::ClipPlayer` (`src/clip_player.{hpp,cpp}`)
- États : `Idle → Loading → Playing → Ended`, + `Error` terminal
- Transitions : `load(path)`, `play()`, `stop()`, `tick(float dt_seconds)`
- Délègue à `HapDemuxer` + `HapDecoder` (composition, pas héritage)
- **Indépendante d'OBS** : aucun appel à `obs_*` dans cette classe. Les
  callbacks OBS resteront dans `hap_clip_source.cpp`. Permet tests unitaires
  purs en stub mode.

**Tests** (`tests/unit/test_clip_player.cpp`, ~8 tests) :
- `CanTransitionIdleToLoading`
- `LoadInvalidPathGoesToError`
- `TickAdvancesPlayingState`
- `TickAtEOFWithLoopGoesBackToStart`
- `TickAtEOFWithoutLoopGoesToEnded`
- `StopReturnsToIdle`
- `DoubleLoadReplacesCurrent`
- `ErrorStateIsTerminal` (ou `ErrorStateRecoverableOnReload` — à décider)

### 1.4.2 — Timing vidéo (FPS pacing)

**Livrables** :
- Dans `ClipPlayer::tick(dt)` : tracker `master_clock_us`, `next_video_pts_us`
- Ne décode la frame suivante que si `master_clock_us >= next_video_pts_us`
- À l'init : `master_clock_us = 0`, `next_video_pts_us = pts_frame_0`
- Après décodage : `next_video_pts_us = next_packet.pts_us`
- En EOF : gestion loop (1.4.4)

**Tests** (~4 tests) :
- `TickBeforeNextPtsDoesNotDecode`
- `TickAtNextPtsDecodesOneFrame`
- `TickBigJumpDecodesOnlyNeededFrames` (pas tout le clip d'un coup)
- `TickRespectsFpsFromVideoInfo`

### 1.4.3 — Audio routing + A/V sync

**Livrables** :
- `ClipPlayer::pullAudio(samples_requested)` → buffer PCM
- Dans `hap_clip_source.cpp` :
  - Soit `audio_render` callback (pull model OBS)
  - Soit push via `obs_source_output_audio_data` depuis video_tick
- **Décision recommandée** : push model. Depuis `video_tick`, lire les packets
  audio dus (selon `master_clock_us`) et les pousser à OBS.
- Conversion format audio container → `obs_source_audio` (planar float 32-bit,
  48 kHz si resampling nécessaire — ou garde sample_rate natif si OBS l'accepte).
- `master_clock_us` piloté par l'audio poussé (audio master).

**Tests** (~4 tests) :
- `PullAudioReturnsSamplesInPlaying`
- `PullAudioEmptyInIdle`
- `AudioPtsDrivesVideoClock` (avance la clock → déclenche décodage vidéo)
- `PullAudioAtEOFWithLoopWrapsAround`

### 1.4.4 — Loop + EOF

**Livrables** :
- En EOF vidéo + `loop=true` : reopen demuxer (`demuxer->reopen(path)`),
  reset `master_clock_us = 0`, `next_video_pts_us = pts_frame_0`, repart en
  `Playing`.
- En EOF + `loop=false` : état `Ended`, frame fixée sur dernière image, audio
  silencieux.
- Compteur de loops (info-only, loggué).

**Tests** (~3 tests) :
- `LoopReopenDemuxerAndResetsClock`
- `NoLoopEndsAtEOF`
- `MultipleLoopsNoMemoryGrowth` (assertion: après 3 loops, decoder texture
  count stable — nécessite hook dans le stub pour compter)

### 1.4.5 — Intégration hap_clip_source + smoke

**Livrables** :
- `hap_clip_context` détient un `ClipPlayer` au lieu de demuxer+decoder directs
- `hap_clip_video_tick` → `player.tick(dt)`
- `hap_clip_video_render` → `player.getTexture()` + draw OBS
- `audio_render` callback ou push depuis tick (selon 1.4.3)
- `update()` : si path change → `player.load(new_path)`
- `activate()` → `player.play()` si autoplay ; `deactivate()` → `player.stop()`
- Ajout `OBS_SOURCE_CONTROLLABLE_MEDIA` flag ? (à vérifier pour exposer
  play/pause via hotkeys plus tard — Phase 4)

**Smoke test manuel** (doc dans `tests/smoke/SMOKE-1.4.md`) :
- Lancer OBS sur Windows + macOS
- Ajouter source DanceHAP Clip → pointer vers `sample_hapa_5s.mov`
- Vérifier : vidéo visible, audio audible, 60s sans crash, loop OK
- Vérifier : `loop=false` → clip s'arrête à la fin proprement
- Capturer 2 screenshots (début + après 30s) pour comparaison visuelle

**Tests** (~3 tests d'intégration stub) :
- `HapClipSourceWithPlayerRendersTexture`
- `HapClipSourcePathChangeReloadsPlayer`
- `HapClipSourceAutoplayStartsOnActivate`

## Impact sur le cahier

- **Nouvel ADR** : `ADR-007-audio-master-clock.md` (stratégie synchro A/V)
- **ARCHITECTURE.md §4.1** : mettre à jour le schéma `hap_clip_source` pour
  montrer la composition avec `ClipPlayer`
- **ROADMAP.md** : cocher 1.4 + sous-items

## Estimation

Pour Célestin solo, en focus :
- 1.4.1 (ClipPlayer state machine) : ~1 jour
- 1.4.2 (timing vidéo) : ~0.5 jour
- 1.4.3 (audio routing) : ~1.5 jour (le plus dense : conversion format, OBS API)
- 1.4.4 (loop + EOF) : ~0.5 jour
- 1.4.5 (intégration + smoke) : ~0.5 jour

**Total indicatif : ~4 jours**, soit un peu plus que l'estimé initial (2-3 j)
parce que l'A/V sync pousse à extraire une classe `ClipPlayer` propre — choix
qui paiera en Phase 2 (matting) et 3 (overlay).

## Plan d'exécution

1. **Gate A** : Jean-Luc valide ce plan + ADR-007 (ce document).
2. Si validé, Célestin :
   - Branche `feat/phase1.4-render-loop`
   - Implémente 1.4.1 → 1.4.5 en commits atomiques
   - Ouvre PR → Gate C (Alfred relit) → merge `main`
3. Smoke test manuel post-merge par Jean-Luc (OBS physique).
4. Si smoke OK : Phase 1.5 (properties) peut démarrer.

## Questions ouvertes pour Jean-Luc

- **Q1** : valider le pattern « audio master clock » (ADR-007) ? C'est le
  standard OBS mais ajoute de la complexité. Alternative : video master +
  espérons que l'audio suit (risque R2 plus élevé).
- **Q2** : `ClipPlayer` en classe dédiée (recommandé, testable) vs logique
  inline dans `hap_clip_context` (plus simple mais moins testable) ?
- **Q3** : smoke test sur machine physique de Jean-Luc, ou sur VM Windows/macOS
  CI runner (plus reproductible mais setup lourd) ? Recommandation : physique,
  on n'a pas de runner OBS headless stable.
