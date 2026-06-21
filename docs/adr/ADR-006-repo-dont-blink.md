# ADR-006 — Hébergement GitHub DanceHAP

Date : 2026-06-21
Statut : Accepté (révisé — voir « Révision 2026-06-21 » ci-dessous)

## Contexte

Jean-Luc dirige deux entités principales :
- **Don't Blink** : projets créatifs, LED, Alfred, expérimentaux. Anciennement
  hébergés sous le compte GitHub `dont-Blink` (au nom de « Madison Rosen »),
  compte auquel le compte `jgason` de Jean-Luc n'a **pas accès en écriture**.
- **Drop The Spoon** (pas de GitHub orga dédiée à date) : entité business
  cherchant revenus récurrents.

DanceHAP est un projet d'outillage créatif pour la danse/streaming, pas un
produit commercial. Il s'inscrit dans l'écosystème créatif Don't Blink.

Le nom de code « DanceHAP » a été choisi pour sa clarté (danse + codec HAP).

## Décision initiale (superseded)

Repo GitHub : **`dont-Blink/dancehap`** (public).

- Organisation : `dont-Blink`
- Nom : `dancehap` (minuscules, pas de tirets supplémentaires)
- Visibilité : public (cohérent avec licence MIT, ADR-005)
- Default branch : `main`
- Admins : `jgason` (Jean-Luc)
- Alfred a accès via le token `jgason` (scopes repo + admin:org)

## Révision 2026-06-21 — hébergement temporaire sous `jgason`

Le compte `dont-Blink` est un **user account** GitHub (pas une orga) au nom de
« Madison Rosen », sur lequel `jgason` n'a aucun droit de création. Aucune orga
`dont-Blink` / `dont-blink` n'existe actuellement.

**Décision révisée** : DanceHAP est hébergé temporairement sous
**`jgason/dancehap`** (public), avec intention explicite de transfert vers une
orga Don't Blink dédiée dès que :

1. Jean-Luc reprend la main sur le compte `dont-Blink` (reset password), ou
2. Une nouvelle orga GitHub `dont-blink` (ou similaire) est créée.

Le transfert via `gh repo transfer` est trivial (zero-downtime, preserve stars,
issues, PRs, et l'URL GitHub redirige automatiquement).

- Repo actuel : `jgason/dancehap`
- Visibilité : public (MIT, ADR-005)
- Default branch : `main`
- Admin : `jgason` (Jean-Luc)
- Alfred a accès via le token `jgason` (scopes repo)

## Conséquences

**Positives** :
- Cohérent avec la marque Don't Blink et la nature créative du projet
- Public dès le départ → visibilité, feedback communauté précoce
- Nom court, mémorable, descriptif
- Pas de friction admin (Jean-Luc possède l'orga)

**Négatives** :
- Aucune significative identifiée

**Neutres** :
- Si DanceHAP devient un produit commercial un jour, fork possible vers une orga
  Drop The Spoon sans renommer (git remote rename suffit)
- Si le projet grossit et nécessite une orga dédiée (`dancehap` org), migration
  facile via GitHub transfer

## Alternatives rejetées

- `drop-the-spoon/dancehap` : pas l'identité du projet (pas un produit commercial)
- Nouvelle orga `dancehap` dès le départ : sur-ingénierie pour un projet naissant
- Nom alternatif (« HapDance », « HapMotion », « DanceDeck ») : moins clair que
  DanceHAP qui dit exactement ce que ça fait
