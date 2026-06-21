# ADR-001 — Plateformes cibles : Windows + macOS

Date : 2026-06-21
Statut : Accepté

## Contexte

DanceHAP vise les streamers et danseurs amateurs. Le marché du streaming se
divise grossièrement :
- **Windows** : ~85-90% des streamers (OBS, matériel gamer, GPUs Nvidia/AMD)
- **macOS** : ~8-10% (créatifs, Apple Silicon de plus en plus présent)
- **Linux** : ~2-3% (techniciens, pas la cible naturelle)

Le codec HAP tourne partout (libhap cross-platform). Le matting ML a des
providers inégaux selon l'OS (CUDA = Nvidia only, DirectML = Windows,
CoreML = macOS, CPU = partout mais lent).

## Décision

Supporter **Windows 10/11 (x64)** et **macOS (Apple Silicon natif + Intel)** au
MVP. Linux est explicitement non-goal jusqu'à signal contraire.

Build matrix CI couvre les deux plateformes dès Phase 1.

## Conséquences

**Positives** :
- Couvre >95% du marché cible sans dispersion d'effort
- macOS Apple Silicon = GPU performant (Metal/CoreML), belle démo
- Windows = marché principal, GPUs Nvidia CUDA = meilleure perf matting

**Négatives** :
- Pas de Linux au MVP : déçoit une partie de la communauté OSS, mais réalité
- Deux chemins de build CMake à maintenir (MSVC + Clang Apple)
- Providers matting doivent être conditionnels (CUDA sur Win, CoreML sur macOS)

**Neutres** :
- Packaging différent par OS (MSI/NSIS sur Win, DMG/notarized sur macOS)
- Doc utilisateur bilingue UI (les deux OS dans les screenshots)
