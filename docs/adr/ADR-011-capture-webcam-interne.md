# ADR-011 — Capture webcam interne DanceHAP

Date : 2026-07-30
Statut : Accepté

## Contexte

DLayer 2 (LIVE) doit accéder au feed webcam pour appliquer le matting.
Trois approches étaient possibles :

1. Référencer une source OBS par nom (`obs_get_source_by_name`)
2. DanceHAP gère sa propre capture webcam interne
3. L'utilisateur duplique la webcam dans OBS

## Décision

**DanceHAP gère sa propre capture webcam interne** (option 2).

Le plugin ouvre directement le device webcam (via l'API de capture vidéo
d'OBS ou via FFmpeg/libavdevice). Pas de référence à une source OBS externe.

### Configuration

Le show file `.dhp` spécifie le device webcam :
```json
"webcam": {
  "device": "default",
  "resolution": "1280x720",
  "fps": 30
}
```

- `"default"` = device par défaut du système
- Ou nom explicite du device (ex: `"HD Pro Webcam C920"`)

## Conséquences

**Positives** :
- Pas de dépendance sur le nom d'une source OBS (plus robuste)
- L'utilisateur n'a pas besoin de configurer une source webcam dans OBS
- Layer 1 (webcam brute) reste optionnel — l'utilisateur peut l'ajouter
  dans OBS comme filet de sécurité, mais DanceHAP ne dépend pas de lui

**Négatives** :
- Le plugin doit gérer le cycle de vie du device (open/close/reconnect)
- Pas de partage avec d'autres sources OBS (si l'utilisateur veut utiliser
  la webcam ailleurs, il doit ajouter une source webcam séparée dans OBS)
- API de capture webcam cross-platform (Windows DirectShow / macOS AVFoundation)

## Alternative rejetée

Référencer une source OBS par nom (option 1) — rejeté car fragile (le nom
de la source peut changer, l'utilisateur peut la supprimer accidentellement)
et ajoute une dépendance cross-source dans OBS.