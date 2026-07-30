# ADR-010 — Architecture 2-composants (éditeur + plugin)

Date : 2026-07-30
Statut : Accepté
Complète : ADR-002 (architecture modulaire 4 briques)

## Contexte

L'architecture v1 (ADR-002) prévoyait 4 briques OBS + un dock Qt lourd
(bibliothèque, compositeur, wizard, config). Le dock faisait trop de choses
dans OBS. Jean-Luc a demandé de scinder en 2 : un éditeur standalone pour
la préparation, et un plugin OBS pour l'exécution.

## Décision

Architecture **2-composants** :

1. **DanceHAP Studio** (C++17 + Qt6) — éditeur standalone, timeline compositor
   avec keyframes B-spline, curve editor, timeline audio, preview. Export `.dhp`.
2. **Plugin OBS DanceHAP** (C++17, OBS MODULE) — source composite qui lit un
   show file `.dhp` et exécute les 3 DLayers en temps réel (fond + live matting +
   overlay) avec dock minimal (play/stop/timecode/markers).

### DLayers (structure interne du plugin)

- **DLayer 1** : HAP background timeline (clips de fond + crossfade + opacité B-spline)
- **DLayer 2** : LIVE (capture webcam interne + matting statique + opacité B-spline)
- **DLayer 3** : HAP overlay timeline (clips alpha + crossfade + opacité B-spline)

### Évolution par rapport à ADR-002

- `hap_clip_source` → intégré comme DLayer 1 dans la source composite
- `ai_matte_filter` → intégré comme DLayer 2 (matting sur capture webcam interne)
- `hap_overlay_source` → DLayer 3 (nouveau)
- `dancehap_dock` → simplifié (dock minimal, pas de bibliothèque ni compositeur)

## Conséquences

**Positives** :
- Toute la complexité UI part dans l'éditeur (pas de dock lourd dans OBS)
- L'éditeur ne link pas libobs — app pure, plus simple à debugger
- Réutilisation : HapDecoder/HapDemuxer partagés entre éditeur et plugin
- Le show file est portable et versionnable

**Négatives** :
- Deux binaires à distribuer (éditeur + plugin)
- La source composite OBS est plus complexe que 4 briques séparées
  (mais le compositing interne est plus simple que 4 entry points OBS)

## Alternative rejetée

Garder ADR-002 (4 briques + dock lourd) — rejeté car le dock Qt dans OBS
était trop complexe pour un plugin, et la préparation de show ne justifie
pas une UI en temps réel.