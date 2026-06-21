# Contribution — DanceHAP

Ce document décrit le workflow de contribution et les **review gates** obligatoires
pour DanceHAP. Tant que la Phase 1 n'est pas livrée, les contributions externes
sont suspendues ; ce document s'applique à l'équipe interne Don't Blink.

## Review gates

Six gates structurent le cycle de vie d'une modification. Tous ne s'appliquent
pas à tous les changements — voir le tableau de déclenchement plus bas.

### Gate A — Plan review

**Quand** : avant d'écrire du code pour une brique ou une feature substantielle.

**Objet relu** : un plan d'implémentation (ex. `docs/plans/PLAN-PHASE-X.md`) qui
détaille :
- Objectif et critère de « done »
- Découpage en étapes testables
- Tests identifiés (unité / intégration / smoke)
- Risques listés
- Impact sur le cahier d'architecture (nouvel ADR ? mise à jour § ?)

**Critère de passage** : le plan est validé par Alfred + Jean-Luc.

### Gate B — Design review

**Quand** : changement impactant >1 module, ou touchant les sections 5 (APIs),
6 (données), ou 7 (sécurité) du cahier.

**Objet relu** : un mini design doc + ADR si décision nouvelle.

**Critère de passage** : ADR accepté par Alfred + Jean-Luc.

### Gate C — Code review

**Quand** : toute PR.

**Objet relu** : le diff complet.

**Checklist reviewer** :
- [ ] Tests verts (unité + smoke si applicable)
- [ ] Pas de secret / credential en clair
- [ ] Gestion d'erreur explicite (pas de throw silencieux, pas de crash OBS)
- [ ] Pas de régression sur les sections impactées du cahier
- [ ] Messages de commit clairs (format conventionnel, voir plus bas)
- [ ] CHANGELOG mis à jour si user-facing

**Critère de passage** : 1 approval d'un reviewer ≠ auteur.

### Gate D — Security review

**Quand** : features touchant à la webcam, au matting, aux modèles ML chargés,
ou à toute donnée potentiellement personnelle.

**Objet relu** : la PR + un mini audit sécurité (où vont les frames webcam ?
le modèle est-il bundlé ou téléchargé ? source du modèle ?).

**Critère de passage** : Alfred valide explicitement la sécurité.

### Gate E — Pre-release review

**Quand** : avant de tagger une release (vX.Y.Z).

**Checklist** :
- [ ] Tous les tests verts en CI (Windows + macOS)
- [ ] CHANGELOG à jour
- [ ] Plan de rollback documenté (désinstallation / downgrade)
- [ ] Smoke test OBS réel passé sur OS cibles
- [ ] Numéro de version bumpé
- [ ] Binaires signés (macOS notarized, Windows code-signing si disponible)

**Critère de passage** : Alfred + Jean-Luc.

### Gate F — Post-mortem

**Quand** : tout incident (crash en prod, régression user-facing, security issue).

**Template** (dans `docs/postmortems/YYYY-MM-DD-incident.md`) :
- Timeline (faits, pas de blame)
- Impact utilisateur
- Cause racine
- Actions correctives (avec propriétaire + date)
- Leçons apprises (à merger dans le cahier si applicable)

## Format des commits

Conventionnel (Conventional Commits) :

```
<type>(<scope>): <description>

<corps optionnel>

<footer optionnel>
```

Types : `feat`, `fix`, `docs`, `refactor`, `test`, `chore`, `build`, `ci`, `perf`.

Exemples :
```
feat(hap_clip_source): implémente décodage Snappy + upload texture DXT5
fix(ai_matte_filter): corrige fuite mémoire provider CUDA
docs(adr): ajoute ADR-007 sur le design system UI
```

## Workflow Git

1. Branche depuis `main` : `feat/<scope>-<sujet>` ou `fix/<scope>-<sujet>`
2. Commits atomiques, un sujet logique par commit
3. Push + PR vers `main`
4. Gate applicable relu
5. Squash-merge vers `main` avec message conventionnel
6. `main` = toujours déployable (vert CI)

## Tests

- **Unité** : GoogleTest (C++), dossier `tests/unit/`
- **Intégration** : dossier `tests/integration/`, OBS headless si possible
- **Smoke** : script `tests/smoke/run_smoke.sh` qui charge OBS + le plugin +
  un HAP de test et vérifie qu'aucun crash

Toute PR doit maintenir ou améliorer la couverture de test existante.

## CI/CD

GitHub Actions, workflows dans `.github/workflows/` :
- `ci.yml` : build + tests sur chaque push/PR (Windows + macOS matrix)
- `release.yml` : packaging + upload release sur tag

À ajouter en Phase 1 (cf. `docs/plans/PLAN-PHASE-1.md`).
