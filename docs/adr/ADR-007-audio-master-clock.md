# ADR-007 — Audio master clock pour la synchro A/V

**Date** : 2026-06-21
**Statut** : Accepté (validé par Jean-Luc, Gate A Phase 1.4)
**Phase** : 1.4 — Boucle de rendu + synchro A/V

## Contexte

DanceHAP lit des clips HAP contenant à la fois un flux vidéo compressé (DXT/BC)
et un flux audio (PCM/AAC). Pour que l'expérience soit correcte, la vidéo et
l'audio doivent rester synchronisés sur la durée — un drift même faible
(> 80 ms après 60 s) casse l'illusion d'un clip « qui joue bien ».

OBS propose plusieurs horloges potentielles :

1. **System clock** (`os_gettime_ns()`) — facile, mais peut drift vs la
   sample-rate audio réelle (48 kHz OBS vs 44.1 kHz container).
2. **Video master** — la vidéo pilote, l'audio suit. Simple mais l'audio peut
   stutter si le decode vidéo a du retard.
3. **Audio master** — l'audio pilote (sample-rate OBS fixe à 48 kHz), la vidéo
   aligne ses PTS sur l'horloge audio. C'est le pattern utilisé par les sources
   media natives d'OBS (`obs-ffmpeg-media.c`).

## Décision

**Audio master clock** : quand le clip a un flux audio, l'audio pilote
l'horloge de lecture. Les frames vidéo sont décodées pour aligner leur PTS sur
cette horloge (pull model piloté par les timestamps audio poussés via
`obs_source_output_audio_data`).

**Fallback** : si le clip n'a pas de flux audio (HAP muet), on bascule sur
system clock (`os_gettime_ns()`) avec la vidéo comme master.

## Implémentation (Phase 1.4)

- `ClipPlayer` maintient `master_clock_us` en interne.
- En mode audio master : `master_clock_us` avance au rythme des échantillons
  audio effectivement poussés à OBS (pas au rythme wall-clock).
- En mode video master (no audio) : `master_clock_us = os_gettime_ns() - start_ns`.
- `ClipPlayer::tick(dt)` décide quelles frames vidéo décoder en comparant
  `master_clock_us` au PTS de la prochaine frame.

## Conséquences

**Positives** :
- Drift A/V borné par la précision de l'horloge audio OBS (très stable).
- Pattern éprouvé (aligné sur OBS media source natif).
- Audio jamais stutter à cause de la vidéo.

**Négatives** :
- Plus complexe que video master (gestion double mode).
- Nécessite de bufferer l'audio en avance pour piloter la clock.

## Alternatives rejetées

- **Video master + system clock** : rejeté car R2 (désynchro) trop élevé sur
  clips longs ; les nuts de sample-rate entre container et OBS s'accumulent.
- **External sync (SMPTE)** : overkill pour un plugin de clip, pas de besoin
  multi-source identifié en Phase 1.

## Révisions

- 2026-06-21 : création.
