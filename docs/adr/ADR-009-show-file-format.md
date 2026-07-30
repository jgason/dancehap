# ADR-009 — Show file format (.dhp JSON)

Date : 2026-07-30
Statut : Accepté

## Contexte

Le plan v2 sépare DanceHAP en 2 composants : un éditeur (DanceHAP Studio)
et un plugin OBS. Le plugin a besoin d'une description complète du show pour
l'exécuter en live. Cette description doit être portable, versionnable, et
indépendante d'OBS.

## Décision

Format **JSON** nommé `.dhp` (DanceHAP Project). Structure :

```
{
  "version": "2.0",
  "show": { name, duration, created },
  "webcam": { device, resolution, fps },
  "matting": { model, threshold, feather, contour, mask_expansion },
  "dlayers": {
    "dlayer1_background": { clips[], opacity_keyframes[] },
    "dlayer2_live": { opacity_keyframes[] },
    "dlayer3_overlay": { clips[], opacity_keyframes[] }
  },
  "audio_tracks": [ { id, file, start, volume } ],
  "markers": [ { time, name } ]
}
```

### Détails

- `matting` est **statique** (1 config pour tout le show, ADR-010)
- `opacity_keyframes` : liste de `{time, value}` (0.0-1.0), interpolation B-spline
  au runtime. Handles de tangente optionnels (`handle_left`, `handle_right`).
- `clips` : liste de clips HAP avec `file`, `start`, `duration`, `loop`,
  `crossfade_in`/`crossfade_out` (durée en secondes, ADR-013).
- `dlayer2_live` : pas de `clips` (feed live). Seulement `opacity_keyframes`.
- `markers` : points de navigation sur la timeline (hybride linéaire + cues).
- `webcam.device` : "default" ou nom du device (ADR-011).

## Conséquences

**Positives** :
- Versionnable (git diff lisible)
- Portable (show file + dossier clips = performance reproductible)
- Pas de réseau entre éditeur et OBS (chargement one-shot)
- Validable (schema JSON, validation chemins)

**Négatives** :
- Pas de modification live du show (il faut recharger le .dhp)
- Chemins absolus (portabilité entre machines limitée)

## Alternative rejetée

Format binaire propriétaire — rejeté car non versionnable et non debuggable.