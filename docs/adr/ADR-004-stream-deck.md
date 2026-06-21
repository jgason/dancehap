# ADR-004 — Stream Deck : hotkeys OBS au MVP, plugin Elgato Phase 5

Date : 2026-06-21
Statut : Accepté

## Contexte

Trois chemins de déclenchement requis : bouton UI, raccourci clavier, et Stream
Deck. Pour le Stream Deck, deux approches :

1. **Plugin Elgato natif** : code C++/JS spécifique à l'API Stream Deck,
   communique avec le plugin OBS via IPC (WebSocket local, pipe, ou shared mem)
2. **Mapping via hotkeys OBS** : l'app Stream Deck officielle (Elgato) mappe ses
   boutons sur des hotkeys système, qu'OBS intercepte. Zéro code Stream Deck.

L'approche 1 offre une meilleure UX (titre dynamique sur le bouton, feedback
état, icône du clip) mais demande du code et du maintien supplémentaires.
L'approche 2 marche tout de suite, gratuitement, mais avec moins de polish.

## Décision

**Au MVP (Phases 1-4) : approche 2 (mapping via hotkeys OBS).**

Chaque clip HAP enregistré expose une hotkey OBS configurable. L'utilisateur
bind cette hotkey dans l'app Stream Deck via l'action « Hotkey ». Documentation
utilisateur fournie avec screenshot.

**Plugin Elgato natif (approche 1) reporté en Phase 5** (polish), conditionné à
validation du MVP et retour utilisateur.

## Conséquences

**Positives** :
- MVP plus court, moins de surface de code, moins de bugs
- Aucune dépendance vendor Elgato au MVP
- L'hotkey OBS marche aussi pour n'importe quel autre contrôleur (MIDI, loupedeck,
  macro keyboard) gratuitement
- Si le MVP échoue ou pivote, pas de code Elgato perdu

**Négatives** :
- UX Stream Deck moins polish au MVP (pas de titre dynamique, pas d'icône clip)
- L'utilisateur doit configurer 2 endroits (DanceHAP + app Elgato)
- Documentation utilisateur obligatoire pour le setup Stream Deck

**Neutres** :
- Si retour utilisateur demande fort le plugin natif, on avancera la Phase 5

## Alternatives rejetées

- **Plugin Elgato natif dès le MVP** : trop de boulot pour une fonctionnalité
  optionnelle. Risque de retarder le MVP pour rien.
- **WebSocket local dédié pour Stream Deck** : sur-ingénierie au MVP, la hotkey
  OBS suffit largement pour déclencher un play/stop.

## Lien avec autres ADRs

- ADR-002 (architecture modulaire) : la hotkey est enregistrée par la source
  HAP-Clip, pas par un manager central, cohérent avec la philosophie modulaire.
