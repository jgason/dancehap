# ADR-013 — Crossfade entre clips HAP

Date : 2026-07-30
Statut : Accepté

## Contexte

Les DLayers 1 et 3 contiennent des timelines de clips HAP. Quand deux
clips se succèdent, une transition est nécessaire. Jean-Luc a demandé du
crossfade configurable.

## Décision

**Crossfade configurable** entre clips HAP successifs sur une même timeline.

### Détails

- Chaque clip a `crossfade_in` et `crossfade_out` (durée en secondes, float).
- `crossfade_out` du clip A + `crossfade_in` du clip B = zone de fondu.
- Pendant le crossfade, les deux clips sont décodés simultanément et blended
  alpha (lerp sur l'opacité).
- Si `crossfade_in`/`crossfade_out` = 0 ou absent → coup sec (instantané).

### Implémentation runtime

- Le ClipPlayer doit pouvoir décoder 2 clips simultanément pendant le
  crossfade (double buffer de frames).
- Le compositing blend : `output = clipA * (1-t) + clipB * t` où `t` va
  de 0 à 1 sur la durée du crossfade.
- La durée du crossfade ne peut pas dépasser la durée restante du clip A
  ni le début du clip B (validation à l'export dans l'éditeur).

## Conséquences

**Positives** :
- Transitions professionnelles sans coup sec
- Configurable par clip (différents tempos selon les numéros)
- Compatible avec l'opacité animable du layer (composé au-dessus)

**Négatives** :
- Double décodage pendant le crossfade (2x CPU/GPU pendant la transition)
- Le ClipPlayer doit gérer 2 clips actifs simultanément
- Validation des chevauchements dans l'éditeur

## Alternative rejetée

Coup sec uniquement — rejeté par Jean-Luc (pas assez fluide pour un spectacle).