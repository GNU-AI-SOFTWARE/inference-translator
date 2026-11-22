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
### Phase 11 — Fidélité, métriques de grounding et boucle de mesure
- [ ] Métriques de grounding dans `/db/status` (citations résolubles, `empty`, latence) — servies par data-base phase 7
- [ ] Pénalité des sorties non ancrées par l'évaluateur quand `grounded: true` (orchestrateur phase 5/7)
- [ ] Acceptation : sur jeu étiqueté, réponse ancrée > réponse libre en fidélité mesurée ; métriques lisibles en POSIX pur

### Phase 10 — `/db` source de vérité pour `/reply` et la recherche
- [ ] `/reply [question]` : réponse = sortie du neurone (champ `output` de `/db`, ou ré-exécution de `/llm1…N` sur les extraits trouvés) ; interroge `/db/training_data` directement (cache NDJSON local = simple miroir en lecture)
- [ ] `/db search MOT…|list [N]|count` servis par la base (data-base phase 7, lectures paginées bornées)
- [ ] Acceptation : cache local vidé → `/reply` répond toujours depuis `/db` ; `cat /db/training_data/<id>` vérifie la citation

### Phase 9 — Prompt `>` ancré dans `/db` (grounding)
- [ ] Extension versionnée des contrats : `request.params.grounded` + `params.context`, `result.sources` + `result.grounded` (`"empty"` si rien dans `/db`)
- [ ] Client : préfixe `?` au prompt `>` ou `/grounded [question]` → recherche par mots-clés dans `/db/training_data` → `write /inference/params` → `/submit`
- [ ] Équivalent POSIX pur : `echo '{"grounded":true}' > /inference/params` + `echo … > /inference/prompt`
- [ ] Réponse calculée par le neurone : prompt `?` + extraits `/db` vectorisés (`inf_text_to_vector`) → `write /llm1…N/input` → lecture `Output:` (`inf_parse_neuron_output`), comme `/entrainement` ; paire {prompt, context, sortie} réenregistrée dans `/db`
- [ ] Orchestrateur : `params.context` distribué aux N instances `/llm1…N`, sorties des neurones agrégées (vote majoritaire), citations `sources` exigées quand `grounded`
- [ ] Acceptation : prompt `? …` → réponse du LLM fondée uniquement sur `/db` avec `source_url`+checksum ; sans correspondance, `grounded: "empty"` ; `make check` vert

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
- [x] `test_client.sh` ne SKIP plus sur Hurd : fallback pty maison (`tests/ptyrun.c`, compilé à la volée) quand `script(1)` est absent (util-linux ne le build pas sur Hurd, Debian #807071) ; `INFERENCE_TEST_FORCE_PTYRUN=1` teste ce chemin sur Linux
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
- [x] `/reply` passe par le neurone quand `/llm1…N` sont montés : question + extrait ancre → `inf_text_to_vector` → `write /llmK/input` → lecture `Output:` (round-robin), repli sur le cache sinon — précurseur de la phase 9 (grounding `?`)
- [x] `/reply` : la sortie du neurone est aussi décodée en texte (`inf_vector_to_text`, inverse d'`inf_text_to_vector`) — bruit tant qu'aucun `.nn` n'est chargé dans `/llm1…N`
- [x] **Trainer externe livré** — `scripts/train-from-db.py` : paires de `/db` (psql) ou du cache NDJSON → backprop sur le MLP sigmoïde (même forward/init que neuron-translator) → `.nn` v2 validé par le vrai `network_load` C → `load` dans `/llm1…N` ; boucle `/db` → `.nn` → neurone → `/reply` fermée
- [x] **Phase 10 commencée** — `/db` source de vérité : `/reply`, `/db search|list|count` lisent `/db` d'abord (nœud `/db/training_data`, protocole `select`, repli `psql`), cache local en secours ; nouveau `/db sync` (fusion dédupliquée par `checksum` dans le cache)

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
