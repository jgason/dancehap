# ADR-012 — Interpolation B-spline pour keyframes d'opacité

Date : 2026-07-30
Statut : Accepté

## Contexte

L'éditeur DanceHAP Studio permet d'animer l'opacité de chaque DLayer dans
le temps via des keyframes. L'interpolation entre keyframes doit produire
des transitions lisses et naturelles, éditables avec des handles de tangente
(façon After Effects / DaVinci Resolve).

## Décision

**Interpolation B-spline** entre keyframes d'opacité.

### Détails

- Chaque keyframe : `{ time, value (0.0-1.0), handle_left?, handle_right? }`
- `handle_left` / `handle_right` : tangentes optionnelles pour B-spline
  non-uniforme. Si absents, B-spline uniforme (lissage automatique).
- Le runtime du plugin calcule l'interpolation B-spline en temps réel
  à partir des keyframes du show file.
- L'éditeur affiche les courbes et permet d'éditer les handles visuellement.

### Implémentation

- Algorithme : B-spline cubique (degré 3) avec clamping aux extrémités.
- Bibliothèque : implémentation custom (algorithme simple, pas de dépendance
  externe) ou lib eigen si déjà disponible.
- Le curve editor Qt utilise QGraphicsScene + QPainter pour le rendu des
  courbes et des handles.

## Conséquences

**Positives** :
- Transitions lisses et naturelles (pas de linéaire robotique)
- Handles éditables = contrôle précis de la courbe
- Format compact dans le show file (juste des points + tangentes)

**Négatives** :
- Plus complexe qu'une interpolation linéaire
- Le runtime doit évaluer la B-spline à chaque frame (coût CPU négligeable)

## Alternative rejetée

Interpolation linéaire — rejeté car pas assez fluide pour des transitions
d'opacité (marches d'escalier visuelles). Bezier cubique — équivalent à
B-spline pour 2 points, mais B-spline gère mieux N points.