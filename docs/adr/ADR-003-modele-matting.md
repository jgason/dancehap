# ADR-003 — Modèle de matting : RVM + fallback MediaPipe

Date : 2026-06-21
Statut : Accepté

## Contexte

Plusieurs modèles de segmentation de personne temps réel existent :

| Modèle | Qualité | Perf GPU | Perf CPU | Poids | Dépendance |
|---|---|---|---|---|---|
| **Robust Video Matting (RVM)** | Excellente (cheveux, mouvements) | Bonne (30-60 fps) | Inutilisable | ~25 Mo | ONNX Runtime |
| **MediaPipe Selfie Segmentation** | Bonne (corps entier) | Très bonne (60+ fps) | Correcte (30 fps) | ~5 Mo | MediaPipe / TFLite |
| **MODNet** | Bonne (portrait) | Bonne | Médiocre | ~20 Mo | ONNX Runtime |
| **BackgroundMattingV2** | Excellente | Lourde | Non | ~200 Mo | PyTorch |
| **U2Net** | Variable | Moyenne | Lente | ~170 Mo | ONNX Runtime |

Le matting doit fonctionner sur GPU dédié (cible 60 fps @1080p) ET offrir un
fallback acceptable pour configs légères (CPU, GPU intégré).

## Décision

**Modèle primaire : RVM (Robust Video Matting)** via ONNX Runtime, avec
execution providers conditionnels :
- CUDA sur Windows/Nvidia
- DirectML sur Windows/AMD ou GPU intégré
- CoreML sur macOS (via ONNX EP CoreML)
- CPU en dernier recours (warning UI)

**Fallback : MediaPipe Selfie Segmentation** pour :
- Configs sans GPU dédié acceptable
- Mode « performance » (qualité réduite, fps max)
- Si l'utilisateur choisit explicitement

L'utilisateur peut forcer le modèle via l'UI du filtre (`ai_matte_filter`).

## Conséquences

**Positives** :
- RVM = meilleur ratio qualité/perf pour la danse (mouvement + cheveux)
- MediaPipe fallback = garantit que ça marche partout, même sur vieux matos
- ONNX Runtime = un seul runtime, plusieurs providers, bien supporté
- Pas de dépendance PyTorch lourde au runtime

**Négatives** :
- Deux modèles à bundler/télécharger (~30 Mo total)
- Deux chemins de test à maintenir
- RVM en CPU = warning obligatoire (sinon mauvaise surprise utilisateur)

**Neutres** :
- Modèles téléchargés post-install (pas dans le binaire) pour garder l'installeur léger
- UI doit exposer le modèle actif + FPS estimé (transparence)

## Alternatives rejetées

- **MODNet** : excellent pour portraits statiques mais moins bon en mouvement
  (or la danse = mouvement)
- **BackgroundMattingV2** : trop lourd (200 Mo), perf GPU exigeante
- **PyTorch runtime** : trop gros pour un plugin OBS
- **Green screen / chroma key classique** : exclut les utilisateurs sans fond vert
  (or la demande est explicitement « sans background », pas « avec fond vert »)
