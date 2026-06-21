# ADR-005 — Licence open source MIT

Date : 2026-06-21
Statut : Accepté

## Contexte

DanceHAP peut être publié sous :
- **MIT / Apache 2.0 / BSD** : permissif, adoption large, contributions communauté
- **GPL / LGPL** : copyleft, oblige les dérivés à ouvrir aussi
- **Propriétaire / commercial** : produit payant Drop The Spoon

Jean-Luc dirige Don't Blink et Drop The Spoon. Le projet s'inscrit dans une
démarche d'outillage créatif (danse, streaming), pas un produit commercial ciblé.

Dépendances tierces prévues :
- OBS Studio : GPL v2.1 (le plugin DOIT donc être compatible GPL au runtime)
- Qt6 : LGPL v3 / commercial
- FFmpeg/libav : LGPL/GPL selon build
- libhap : BSD-3
- ONNX Runtime : MIT
- RVM : MIT
- MediaPipe : Apache 2.0

## Décision

**Licence MIT** pour le code DanceHAP.

Compatible avec toutes les deps ci-dessus (MIT est permissif, autorisé par GPL
et LGPL en combinaison). Adoption maximale, contributions communauté possibles.

## Conséquences

**Positives** :
- Adoption maximale (personne n'hésite à intégrer / forker)
- Compatible OBS (GPL v2.1) car MIT + GPL = OK (GPL domine au runtime)
- Compatible Qt LGPL, FFmpeg, libhap, ONNX, RVM, MediaPipe
- Signature Don't Blink comme acteur OSS de la communauté streaming/créative
- Possibilité de contributions externes (bugfix, nouveaux modèles, traductions)

**Négatives** :
- Pas de revenu direct (pas un produit payant) — OK car pas l'objectif
- Un concurrent pourrait forker — acceptable, la valeur est dans la communauté
  et l'expertise HAP/ML, pas dans le code seul

**Neutres** :
- Pas de CLA (Contributor License Agreement) au démarrage — à revoir si
  contributions significatives d'entreprises
- Copyright holder : « Don't Blink » (visible dans le LICENSE)

## Alternatives rejetées

- **GPL v3** : plus restrictif, décourage certaines intégrations enterprise
- **LGPL** : complexité dynamic linking inutile pour un plugin OBS (déjà GPL)
- **Apache 2.0** : excellent aussi, mais MIT plus court à lire et suffisant ici
- **Commercial / propriétaire** : pas l'objectif, et empêche l'adoption
