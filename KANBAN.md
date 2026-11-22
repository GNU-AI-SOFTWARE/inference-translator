<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

Kanban de l'Inference Translator, dérivé du PLAN.md : une carte
par tâche. Déplacer une carte = la déplacer entre les sections
ci-dessous. Le détail des livrables et des critères d'acceptation
reste dans PLAN.md.
-->

# Inference Translator — Kanban

Dérivé du [`PLAN.md`](PLAN.md). Chaque carte est préfixée par sa
phase ; les critères d'acceptation de chaque phase sont dans le plan.

## À faire
### Phase 8 — Session distante de production (post-v1.0)
- [ ] Reconnexion/reprise : trame `request` rejouée sans doublon en base après coupure
- [ ] Multi-utilisateurs simultanés (un `inference-serveur` par session SSH, registre de clés pour N utilisateurs)
- [ ] IHM de production : progrès de la requête, état du cluster depuis `/orchestrate`


### Phase 3 — Interface riche : couleurs, animations, streaming
- [ ] Mode cluster : le trio JSON transite par un canal SSH minimal vers l'orchestrateur d'un cluster Hurd (2 à 5 nœuds) — reporté avec la phase 5 (mode distant)

### Phase 4 — Intégration de bout en bout avec l'orchestrateur
- [ ] Scénario complet : prompt (éditeur) → `request` → orchestrateur (httpfs, N neuron-translator) → `result` affiché en streaming — bloqué par la phase 3 d'`orchestrator-translator` (lecture de `/inference/request`) ; côté client, `/submit` + `/watch` prêts
- [x] Affichage du `status` (instances, états) comme données de progression et du `result` en streaming : commande `/watch` (#54)

### Phase 5 — Mode distant : SSH, datacenter et cluster local
- [ ] Processus `inference-serveur` lancé par session SSH (trames sur stdio, aucun port d'écoute dédié)
- [ ] Registre des clés publiques SSH via PostgreSQL (`data-base-translator`) : enregistrement, `authorized_keys`, révocation côté serveur, journal des refus
- [ ] Client : `inference --remote utilisateur@hôte` ; sans l'option, comportement local inchangé

### Phase 6 — Historique et ergonomie de session
- [x] Historique des lignes de la session persistant dans `.history` (`$HOME/.history` par défaut, `--history FILE` / `$INFERENCE_HISTORY`), rappel par flèche haut/bas comme un terminal TTY, commande `/history` (#56)
- [ ] Ré-édition d'une entrée rappelée dans `$EDITOR` et re-soumission (`/replay N`)
- [ ] Commande `replay` cohérente avec `/orchestrate`, sans dupliquer la persistance (PostgreSQL reste la source)

### Phase 7 — Durcissement, tests, CI
- [ ] Tests déterministes du translator (suites POSIX sans terminal), tests du client via pseudo-terminaux
- [ ] Tests du mode distant : sshd de test sur boucle locale, clé refusée, révocation à chaud, coupure de session
- [ ] CI sous QEMU GNU/Hurd, pilotée par [GNU-AI-SOFTWARE/mistral-vm-debian-hurd](https://github.com/GNU-AI-SOFTWARE/mistral-vm-debian-hurd)
- [ ] Documentation (`docs/interface.md`, `docs/remote.md`, `docs/architecture.md`)

## En cours

_(rien)_

## Fait

### Phase 3 — Interface riche : couleurs, animations, streaming
- [x] Coloration syntaxique du prompt et des sorties JSON (#49)
- [x] Animations : spinner pendant `/submit`, réaffichage propre ; retour arrière UTF-8 corrigé (#49)
- [x] Commandes d'entraînement : `/learn` (#50), `/read-code` (#52), `/learn-code` (#53) — persistance `/db` (`training_data`, checksum SHA-256)

### Phase 2 — Le client `inference` : IHM minimale
- [x] Bannière colorée, saisie multi-lignes (`termios`), commandes de base (`/editor`, `/task`, `/submit`, `/status`, `/quit`) (#11)
- [x] Intégration `$EDITOR` : composition du prompt dans nano/vim/emacs, relecture du buffer, re-soumission (#12)
- [x] Narration des actions avec le nœud POSIX concerné à chaque étape (#13)

### Phase 0 — Spécification et contrats
- [x] Gel du trio JSON `request`/`status`/`result` et de l'arborescence `/inference` (SPEC.md §1–§2)
- [x] Gel du mode distant : transport SSH (commande serveur, session), format de trame, poignée de main `hello`, version, registre des clés publiques SSH (SPEC.md §7)
- [x] Protocole éditeur : résolution `$VISUAL` → `$EDITOR` → `nano` → `vi`, fichier temporaire, sémantique du code de sortie (SPEC.md §5)
- [x] Palette et conventions d'affichage (codes ANSI, thèmes de base, réglage `NO_COLOR`) (SPEC.md §6)
- [x] Convention des points de montage : `/inference`, `/orchestrate` (SPEC.md §4)
- [x] Livrable : `SPEC.md` + squelette de code compilable (cœur C23 pur + tests 36/36 + prototype CLI ncurses `--cli`)

### Phase 1 — MVP : le translator `/inference` (mode local)
- [x] Translator monté via `settrans` sur `/inference` (libnetfs, `ON_HURD` : `main-hurd.c` + `netfs-hooks.c`) — #7
- [x] `write` du prompt sur `/inference/prompt` ; extraction des URLs (regex gelée) — #8
- [x] `read` de `/inference/request` (type 1), `/urls`, `/task`, `/status`, `/params`, `/version` — #9
- [x] Détection heuristique de la tâche (mots-clés), surcharge manuelle via `/inference/task` — #10
- [x] Reset de la surcharge : écrire `auto` sur `/inference/task` re-détecte la tâche depuis le prompt courant (`/task auto` côté client, complétion Tab) — rend `test_posix.sh` idempotent sur un montage persistant
