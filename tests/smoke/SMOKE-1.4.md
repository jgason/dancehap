# Smoke Test — Phase 1.4 (Render Loop + A/V Sync)

> **Statut** : À exécuter post-merge par Jean-Luc sur machine physique.
> **Prérequis** : Plugin compilé en mode real-OBS (DANCEHAP_WITH_OBS_DEPS=ON)
> avec FFmpeg + Snappy, installé dans OBS.

## Objectif

Vérifier que la source « DanceHAP Clip » produit une vidéo visible + un audio
audible en synchro, en boucle, sans crash — sur Windows ET macOS.

## Asset de test

`tests/assets/sample_hapa_5s.mov` — HAPA 256×256, 30 fps, 5 s, AAC 48 kHz stereo.

## Procédure

### Étape 1 — Chargement

1. Lancer OBS Studio (≥ 31).
2. Ajouter une source : **Ajouter → DanceHAP Clip**.
3. Dans les propriétés, pointer vers `sample_hapa_5s.mov`.
4. Laisser **Loop** activé, **Autoplay** désactivé.

**Résultat attendu** :
- La preview OBS affiche une vidéo non-noire (la première frame du clip).
- Aucun crash, aucun message d'erreur dans le log OBS.
- La dimension de la source est 256×256.

### Étape 2 — Lecture en boucle (loop=true)

1. La source est active (visible dans la scène).
2. Observer pendant **60 secondes minimum**.

**Résultat attendu** :
- La vidéo joue en continu, en boucle, sans stutter visible.
- L'audio est audible via le mixer OBS (piste « DanceHAP Clip »).
- Pas de drift A/V perceptible (> 80 ms) après 60 s.
- Pas de croissance mémoire observable (Task Manager / Activity Monitor).

**Capture** : prendre 2 screenshots — un à t=0s, un à t=30s — pour comparaison.

### Étape 3 — Fin de clip (loop=false)

1. Ouvrir les propriétés de la source.
2. Décocher **Loop**.
3. Observer la fin du clip.

**Résultat attendu** :
- Le clip joue une fois puis s'arrête proprement.
- La dernière frame reste fixée à l'écran (pas d'écran noir).
- Aucun crash.

### Étape 4 — Changement de clip en cours de route

1. Ouvrir les propriétés.
2. Changer le chemin vers un autre clip HAP (ou re-sélectionner le même).
3. Vérifier que le nouveau clip joue immédiatement.

**Résultat attendu** :
- Le clip change sans crash ni gel.
- L'audio et la vidéo du nouveau clip démarrent correctement.

## Critères de passage

| # | Critère | Méthode de vérification |
|---|---------|------------------------|
| 1 | Vidéo visible non-noire | Inspection visuelle preview |
| 2 | Audio audible | Mixer OBS, niveau non nul |
| 3 | Synchro A/V < 80 ms drift à 60 s | Inspection visuelle (lip-sync) |
| 4 | Loop sans stutter ni fuite | 60 s + Activity Monitor |
| 5 | loop=false → arrêt propre | Dernière frame fixée |
| 6 | Pas de crash sur les 4 étapes | Log OBS sans exception |

## Notes techniques

- **Audio master clock (ADR-007)** : l'audio pilote l'horloge de lecture.
  La vidéo aligne ses PTS sur l'horloge audio.
- **Phase 1.4 limitation** : `pullAudio()` retourne du silence en stub mode
  (pas de décodage AAC intégré). En real-OBS mode avec FFmpeg, l'audio AAC
  doit être décodé en PCM float planar pour être réellement audible.
  L'infrastructure de push audio (`obs_source_output_audio_data`) est en
  place ; le décodage audio réel est un travail Phase 1.5+.
- Si l'audio n'est pas audible mais la vidéo est correcte, vérifier que le
  format audio est bien décodé (FFmpeg avec libfdk_aac ou aac natif).
