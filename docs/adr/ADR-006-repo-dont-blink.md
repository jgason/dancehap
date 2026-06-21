# ADR-006 — Repo `dont-Blink/dancehap`

Date : 2026-06-21
Statut : Accepté

## Contexte

Jean-Luc dirige deux entités principales :
- **Don't Blink** (GitHub orga `dont-Blink`) : projets créatifs, LED, Alfred,
  expérimentaux. Compte GitHub `jgason` avec accès admin org.
- **Drop The Spoon** (pas de GitHub orga dédiée à date) : entité business
  cherchant revenus récurrents.

DanceHAP est un projet d'outillage créatif pour la danse/streaming, pas un
produit commercial. Il s'inscrit dans l'écosystème créatif Don't Blink.

Le nom de code « DanceHAP » a été choisi pour sa clarté (danse + codec HAP).

## Décision

Repo GitHub : **`dont-Blink/dancehap`** (public).

- Organisation : `dont-Blink`
- Nom : `dancehap` (minuscules, pas de tirets supplémentaires)
- Visibilité : public (cohérent avec licence MIT, ADR-005)
- Default branch : `main`
- Admins : `jgason` (Jean-Luc)
- Alfred a accès via le token `jgason` (scopes repo + admin:org)

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
