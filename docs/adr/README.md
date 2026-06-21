# Architecture Decision Records

Ce dossier contient les décisions d'architecture (ADR) de DanceHAP, au format
[Nygard](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions).

Chaque ADR est immutable : si une décision est revisité, on crée un nouvel ADR
qui supersede l'ancien (avec un lien dans l'ancien).

## Index

| ADR | Titre | Statut |
|---|---|---|
| [ADR-001](ADR-001-plateformes-cibles.md) | Plateformes cibles : Windows + macOS | Accepté |
| [ADR-002](ADR-002-architecture-modulaire.md) | Architecture modulaire (4 briques) | Accepté |
| [ADR-003](ADR-003-modele-matting.md) | Modèle de matting : RVM + fallback MediaPipe | Accepté |
| [ADR-004](ADR-004-stream-deck.md) | Stream Deck : hotkeys OBS au MVP, plugin Elgato Phase 5 | Accepté |
| [ADR-005](ADR-005-licence-mit.md) | Licence open source MIT | Accepté |
| [ADR-006](ADR-006-repo-dont-blink.md) | Hébergement GitHub : `jgason/dancehap` (transfert prévu) | Accepté (révisé) |

## Format d'un ADR

```markdown
# ADR-NNN — Titre court

Date : YYYY-MM-DD
Statut : Accepté (ou Proposé / Superseded par ADR-XXX / Déprécié)

## Contexte

Pourquoi cette décision se pose-t-elle ? Quelles sont les forces en présence ?

## Décision

Qu'a-t-on décidé, en une phrase claire ?

## Conséquences

Positives, négatives, neutres. Risques acceptés.
```
