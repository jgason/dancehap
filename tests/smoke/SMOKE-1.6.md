# Smoke Test — Phase 1.6 (CI Matrix Final + Sortie Phase 1)

> **Statut** : Windows validé 2026-06-24 par Jean-Luc. macOS à exécuter.
> **Version** : v0.3.2 (PR #8 + #9 merged sur main).
> **Prérequis** : Plugin installé dans OBS (DLL Windows ou `.plugin` macOS).

## Objectif

Valider le critère de sortie Phase 1 : on peut ajouter une source
« DanceHAP Clip » dans OBS, pointer vers un .mov HAP, et voir la vidéo +
entendre l'audio en synchro, sur Windows ET macOS.

## Assets de test

- `tests/assets/sample_hapa_5s.mov` — HAPA 256×256, 30 fps, 5 s, AAC 44.1 kHz mono
- `tests/assets/sample_hapa_5s_pcm.mov` — HAPA 256×256, 30 fps, 5 s, PCM s16le 44.1 kHz mono

## Scripts automatisés

- **Windows** : `powershell -ExecutionPolicy Bypass -File tests/smoke/run_smoke_windows.ps1`
  Vérifie DLL installée, version embarquée, symbole `obs_module_load`, asset présent.
- **macOS** : `bash tests/smoke/run_smoke_macos.sh`
  Vérifie `.plugin` installé, version embarquée, symbole `obs_module_load`, asset présent.
- **Symbol check (CI)** : `bash tests/smoke/check_source_registered.sh <plugin>`

Les scripts automatisés ne remplacent pas le smoke manuel dans OBS — ils
vérifient que l'install est saine avant de lancer OBS.

## Procédure manuelle (Windows + macOS identiques)

### Étape 1 — Chargement

1. Lancer OBS Studio (≥ 31).
2. Ajouter une source : **Ajouter → DanceHAP Clip v0.3.2**.
3. Dans les propriétés, pointer vers `sample_hapa_5s.mov`.
4. Laisser **Loop** activé, **Autoplay** désactivé, **Volume** à 100.

**Résultat attendu** :
- La preview OBS affiche la vidéo HAP (damier pink/blue, pas de barres verticales).
- Le nom de la source contient `v0.3.2` (confirmation de la DLL loaded).
- Aucun crash, aucun message d'erreur dans le log OBS.
- La dimension de la source est 256×256.

### Étape 2 — Lecture en boucle (loop=true, AAC audio)

1. La source est active (visible dans la scène).
2. Observer pendant **60 secondes minimum**.

**Résultat attendu** :
- La vidéo joue en continu, en boucle, sans stutter visible.
- L'audio AAC est audible via le mixer OBS (piste « DanceHAP Clip »).
- Pas de drift A/V perceptible (> 80 ms) après 60 s.
- Pas de croissance mémoire observable (Task Manager / Activity Monitor).

### Étape 3 — Audio PCM (loop=true, PCM audio)

1. Ouvrir les propriétés de la source.
2. Changer le chemin vers `sample_hapa_5s_pcm.mov`.
3. Vérifier que l'audio est audible (PCM s16le via le même path FFmpeg).

**Résultat attendu** :
- Vidéo identique (même clip source, audio ré-encodé).
- Audio audible, pas de coupure, pas de crash.

### Étape 4 — Fin de clip (loop=false)

1. Ouvrir les propriétés de la source.
2. Décocher **Loop**.
3. Observer la fin du clip.

**Résultat attendu** :
- Le clip joue une fois puis s'arrête proprement.
- La dernière frame reste fixée à l'écran (pas d'écran noir).
- Aucun crash.

### Étape 5 — Changement de clip en cours de route

1. Ouvrir les propriétés.
2. Changer le chemin vers un autre clip HAP (ou re-sélectionner le même).
3. Vérifier que le nouveau clip joue immédiatement.

**Résultat attendu** :
- Switch propre, pas de crash, pas de frame résiduelle.

### Étape 6 — Volume

1. Ouvrir les propriétés.
2. Changer **Volume** de 100 → 0 → 50.
3. Vérifier que le niveau audio suit ( mixer OBS ou耳朵).

**Résultat attendu** :
- Volume 0 = muet, 50 = moitié, 100 = plein.
- Pas de clic/pop audible lors du changement.

## Critère de sortie Phase 1

Toutes les étapes 1-6 passent sur **Windows ET macOS** sans crash, avec vidéo +
audio en synchro. Windows validé 2026-06-24. macOS reste à valider.

## Rollback

Si la DLL v0.3.2 casse quelque chose, restaurer la backup :
- Windows : `copy "C:\Program Files\obs-studio\obs-plugins\64bit\dancehap.dll.bak_v0.3.1_renderbars_fixed" "C:\Program Files\obs-studio\obs-plugins\64bit\dancehap.dll" /Y`
- macOS : restaurer l'ancien `.plugin` bundle.