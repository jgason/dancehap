# ADR-002 — Architecture modulaire (4 briques)

Date : 2026-06-21
Statut : Accepté

## Contexte

Deux approches possibles pour DanceHAP :
1. **Mono-source** : une seule source OBS géant qui fait tout (HAP + webcam +
   matting + overlay + audio) avec une UI complexe
2. **Modulaire** : 4 briques séparées (source HAP, filtre matte, source overlay,
   dock UI) que l'utilisateur compose dans OBS

La mono-source semble plus simple pour l'utilisateur débutant (« un bouton qui
fait tout ») mais pose des problèmes : impossible d'utiliser la webcam ailleurs,
impossible de désactiver le matting si GPU faible, réinvente le compositing OBS.

## Décision

Architecture **modulaire** : 4 briques exposées comme objets OBS natifs
distincts. L'utilisateur compose sa scène via le pipeline standard OBS.

## Conséquences

**Positives** :
- Chaque brique testable sola, réutilisable dans d'autres projets
- L'utilisateur garde le contrôle total (peut enlever le matting, remplacer la
  webcam par une capture écran, etc.)
- On ne réinvente pas l'audio monitoring, les transitions, le multistream — OBS gère
- Défaillance d'une brique n'empêche pas les autres de fonctionner
- Évolution incrémentale : peut ajouter une brique « chroma key » plus tard sans
  toucher aux autres

**Négatives** :
- Setup initial pour l'utilisateur final = quelques étapes (ajouter 3 sources +
  filtre + dock). Mitigation : template de scène OBS fourni + wizard
- Plus de surface de code / d'API à maintenir (4 entry points OBS au lieu d'1)
- Coordination entre briques via signaux/propriétés OBS (pas de shared state
  direct) → un peu de glue

**Neutres** :
- Nécessite un template scène + wizard de premier démarrage pour rester
  « TRES user friendly » comme demandé
- Documenter le « comment composer sa scène » dans la doc utilisateur

## Alternative rejetée

Mono-source géant avec onglets internes. Rejeté car :
- Couplage fort, testing difficile
- Verrouille l'utilisateur dans notre pipeline
- Risque de casser tout si une brique bug
- Contredit la philosophie modulaire d'OBS
