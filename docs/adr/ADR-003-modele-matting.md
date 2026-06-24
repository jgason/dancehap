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
- **DirectML sur Windows (tous GPU)** — Nvidia, AMD et Intel iGPU via DirectX 12.
  Un seul binaire couvre tout l'écosystème Windows, sans dépendance CUDA.
- CoreML sur macOS (via ONNX EP CoreML, Metal)
- CPU en dernier recours (warning UI)

**Fallback : MediaPipe Selfie Segmentation** pour :
- Configs sans GPU dédié acceptable
- Mode « performance » (qualité réduite, fps max)
- Si l'utilisateur choisit explicitement

L'utilisateur peut forcer le modèle via l'UI du filtre (`ai_matte_filter`).

### Rationale du pivot CUDA → DirectML (révision 2026-06-24)

La décision initiale listait CUDA (Nvidia), DirectML (AMD/Intel), CoreML (macOS).
Après analyse, **CUDA est retiré** au profit de DirectML comme provider Windows
unique :

- **Couverture GPU** : DirectML couvre Nvidia + AMD + Intel via DirectX 12.
  CUDA ne couvre que Nvidia → verrouillerait les users AMD/Intel.
- **Setup/distribution** : DirectML.dll (~5 Mo, NuGet `Microsoft.ML.OnnxRuntime.DirectML`)
  vs CUDA Toolkit 12 (~3 Go, version-locked, lourd à bundler en CI).
- **Maturité RVM** : RVM MobileNetV3 n'utilise que des ops standard (conv2d,
  batchnorm, relu, upsampling) parfaitement supportées par DirectML. Validé par
  la communauté RVM et par Stable Diffusion (qui tourne sur DirectML en prod).
- **Perf** : DirectML atteint 40-60 fps sur RVM @1080p (vs 60+ pour CUDA).
  Le delta de perf ne justifie pas le coût de distribution de CUDA pour un
  plugin OBS grand public.
- **Vulkan EP** : envisagé mais rejeté — expérimental, ops manquantes pour RVM,
  pas mature en 2026.

**CUDA n'est rejeté que pour la distribution**. Un utilisateur avancé peut
toujours rebuilder le plugin avec `DANCEHAP_HAVE_CUDA` s'il veut la perf max
sur sa machine Nvidia, mais le binaire officiel n'inclura pas CUDA.

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
