<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Inference Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# CHANGELOG — Inference Translator

Format « Keep a Changelog », versionnage sémantique. Les contrats gelés
de `SPEC.md` ne changent jamais silencieusement : toute évolution passe
par un bump de `version`.

## [0.1.1] — 2026-11-22

## [Unreleased]

### Modifié

- **Reset de version à 0.1.0** : la v1.0.0 avait été coupée par le tail d'un
  bootstrap raté (avant le merge des PRs de versioning). `VERSION` repasse à
  0.1.0, la section `## [1.0.0]` est retirée et le tag `v1.0.0` supprimé ;
  le versionnage itératif repart de 0.1.0 → 0.1.1 → …

### Corrigé

- **Build** : `install.sh` déplacé vers `scripts/install.sh` — la racine
  étant le aux-dir d'autoconf, automake ≥ 1.17 transformait
  « 'install.sh' is an anachronism » en erreur dure et cassait
  `autoreconf -i` / `autogen.sh`. Le tarball injecté dans la VM partait
  alors avec des `Makefile.in` périmés et `make` y mourait sur
  `missing automake-1.17`. `Makefile.am`, `configure.ac`, `INSTALL.md`,
  `INSTALL-EN.md` mis à jour.
- **hurd-vm** : `guest-inference.sh` exige désormais un système de build
  pré-généré complet (`configure`, `Makefile.in`, `config.h.in` —
  marqueur `GUEST-BUILD-KO` sinon) et normalise les timestamps après
  extraction : `make` ne tente plus jamais de régénérer `configure` /
  `Makefile.in` dans le guest (image sans autotools).

- **pty de test (GNU/Hurd)** : `test_client.sh` échouait sur Hurd via ptyrun.
  Deux mécanismes Hurd en cause : (a) tant que rien n'a été écrit sur le
  master, le côté esclave ne progresse pas (les variantes qui retenaient
  l'stdin en attendant la première sortie du fils produisaient des sessions
  pty complètement vides) ; (b) au clear de `ICANON`, le serveur pty Hurd
  peut jeter la file canonique, d'où la perte de la première ligne pipée
  (`/submit` à buffer vide). Correctifs : `tests/ptyrun.c` revient à
  l'écriture immédiate sur le master (plus de gate, plus de timeout de
  poll) avec un mode `PTYRUN_DEBUG=1` ; `src/client.c` draine avant le
  `tcsetattr` raw l'entrée déjà arrivée (`FIONREAD` + `read` en mode
  canonique, sans bloquer) dans un stash que `read_line` consomme d'abord —
  la 1re ligne pipée survit donc au flush. `tests/test_client.sh` dump les
  captures pty (`cat -v`) dans le log en cas d'échec. Vérifié en local
  (Linux) : ptyrun forcé et `script(1)` OK.

- **`tests/test_fs.c`** : le nœud `version` était comparé à une version
  codée en dur (`"0.1.0"`) — le test cassait à chaque bump (v1.0.0). Il
  attend désormais la macro `VERSION` (via `PACKAGE_VERSION`/`config.h`),
  comme le rend `src/fs.c`. Vérifié : 29/29 avec `PACKAGE_VERSION` absent,
  `1.0.0` et `0.1.2`.

### Ajouté

- **Versionnage itératif** : `scripts/release.sh iterate` (bump patch auto à
  chaque itération : 0.1.0 → 0.1.1 → 0.1.2 …, `--all` pour toute la pile),
  helper `scripts/version.sh` (`--next`, `--check`), doc `docs/VERSIONING.md`
  (section « Versionnage itératif »).
- **Phase 3 (client riche)** : coloration syntaxique du prompt et des JSON
  (`inf_json_highlight`, `inf_text_highlight`), spinner pendant `/submit`,
  statuts colorés, retour arrière UTF-8 corrigé (`del_char`),
  redessin atomique de la ligne.
- **`/learn [URL...]`** : enregistre les pages téléchargées comme
  éléments d'entraînement dans `/db` (table `training_data` :
  `source_url`, `http_status`, `content`, `checksum` SHA-256 anti-doublon) ;
  `--db DIR` / `$INFERENCE_DB`, SHA-256 autonome et `inf_html_to_text` dans
  le cœur pur.
- **`/read-code URL`** : détection du langage (extension puis shebang) et
  aperçu coloré (`inf_lang_detect`, `inf_code_highlight`) ;
  `inf_url_trim` retire la ponctuation finale des URL (`https://x.org,`).
- **`/learn-code URL`** : `/read-code` puis persistance du code dans `/db`.
- **Phase 4 (côté client) `/watch`** : surveille `/orchestrate`
  (`--orchestrate DIR` / `$INFERENCE_ORCH`) : polling du statut (type 2,
  instances affichées en couleur) jusqu'à l'état terminal, puis
  résultat agrégé (type 3) en JSON coloré avec `output` affiché
  façon machine à écrire et `confidence` colorée. Lecture des
  sous-nœuds `/orchestrate/{status,result}` (phase 2+) avec repli sur le
  protocole de commandes phase 1 (`{"command":"status"}`). L'interface
  n'écrit jamais de descripteur de run dans `/orchestrate` (contrat gelé).
- **Extraction JSON** dans le cœur pur : `inf_json_str_field`,
  `inf_json_num_field` (tests 51/51, contrat 36/36 — 12 commandes slash).
- **Phase 6 (historique de session)** : chaque ligne acceptée est
  enregistrée dans `$HOME/.history` (défaut ; `--history FILE` /
  `$INFERENCE_HISTORY`, mode 0600, dédoublonnage des doublons consécutifs
  façon `HISTCONTROL=ignoredups`) et rappelable par **flèche haut/bas**
  comme un terminal TTY (la ligne en cours est préservée puis restaurée) ;
  commande **`/history`** (13e commande slash) ; cœur pur
  `inf_history_*` testé (64/64, contrat 36/36 — 13 commandes).
- **`/uptime`** (14e commande slash) : version du client, version du
  translator (lue dans `<montage>/version`) et uptime de la session ;
  formateur pur `inf_format_uptime` testé (106/106, contrat 36/36 — 19
  commandes). Documentation à jour : `inference(1)` (options `--db`,
  `--orchestrate`, `--history`, commandes `/learn` `/read-code`
  `/learn-code` `/watch` `/history` `/uptime`, flèches haut/bas,
  variables `INFERENCE_DB`/`INFERENCE_ORCH`/`INFERENCE_HISTORY`).
- **`/reply [question]`** (15e commande slash) : répond selon les données
  d'entraînement — recherche par mots-clés (`inf_training_search`) dans le
  cache local NDJSON (`$INFERENCE_TRAINING`, défaut
  `$HOME/.inference-training.ndjson`), rempli par `/learn`, `/learn-code`
  et `/entrainement` ; les enregistrements `/entrainement` répondent par la
  sortie du neurone (champ `output`).
- **`/sigmoid`** (16e commande) : visualise l'état des neurones montés
  `/llm1..N` — sorties courantes lues sur chaque nœud, activation sigmoïde
  et barre ASCII par sortie (`inf_parse_neuron_output`, `inf_sigmoid`,
  `inf_sigmoid_render`, nouveau module `src/neuron.c`).
- **`/entrainement URL|hf://… [dir]`** (17e commande) : télécharge la page
  (ex. `https://huggingface.co/datasets`) **ou un dépôt `hf://`** (ex.
  `hf://buckets/clairemariecoralie/cyber-security-bucket`, fichiers
  téléchargés dans `[dir]`, défaut `./data`, comme `hf sync ./data hf://…`),
  découpe en extraits, fait passer chaque extrait par le
  neuron-translator (`input` vectorisé déterministiquement via
  `inf_text_to_vector`, lecture de la sortie), puis enregistre
  `{input, output, neuron}` dans `/db` (`training_data`) et le cache local.
- **DNS automatique** : à chaque URL ajoutée (`/fetch`, `/learn`,
  `/read-code`, `/learn-code`, `/entrainement`), l'hôte est extrait
  (`inf_url_host`), et s'il n'est ni une IP littérale ni présent dans
  `/etc/hosts` (`inf_hosts_has`), il y est ajouté après résolution `getent`
  — httpfs atteint la cible même sans DNS fonctionnel dans la VM (le
  « status HTTP: 0 » de `/learn` sur une VM sans DNS).
- **Ligne d'information machine** : à chaque prompt, une ligne atténuée
  `CPU … | MEM …` (`inf_client_stats_line`) s'affiche entre la ligne de
  prompt et le reste de la fenêtre.
- **`/hf`** (18e commande) : `/hf list hf://…` liste l'arbre d'un dépôt
  Hugging Face (buckets/datasets/models) via l'API HTTP
  (`inf_hf_parse`, `inf_hf_api_url`, `inf_hf_resolve_url`,
  `inf_json_array_count`/`inf_json_array_object`) ; `/hf sync
  hf://buckets/<owner>/<bucket> ./local` télécharge chaque fichier vers le
  dossier local (copie sûre, sans `..`) puis le **convertit en données
  lisibles par la base** : découpage, SHA-256, enregistrements `/db`
  `training_data` + cache local. Fonctionne par httpfs pur (pas besoin du
  CLI `hf` installé via `curl -LsSf https://hf.co/cli/install.sh | bash`,
  qui reste une alternative pour les gros volumes).
- **`/db`** (19e commande) : exploration/recherche de la base —
  `/db status` (écrit `{"status": true}` sur `/db`, affiche la réponse),
  `/db search MOT...` (liste les enregistrements correspondants,
  `inf_training_search_list`), `/db list [N]`, `/db count`.

### Corrigé

- **Mémoire affichée à 100 % sur Hurd** : les pages `inactive` de Mach sont
  désormais comptabilisées comme disponibles (`free_count + inactive_count`)
  dans `/stats` **et** dans la ligne d'information au-dessus du prompt.
- **`hf://buckets/…` cassé côté URLs** : les buckets HF sont sans révision —
  `/api/buckets/<o>/<r>/tree/main` répond `[]` et
  `/buckets/<o>/<r>/resolve/main/…` répond 404. `inf_hf_api_url` utilise
  désormais `…/api/buckets/<o>/<r>/tree?recursive=true` et `inf_hf_resolve_url`
  `…/buckets/<o>/<r>/resolve/<path>` pour `buckets` (datasets/models gardent
  `/tree/main` et `/resolve/main`). Le paramètre `prefix` étant ignoré par
  l'endpoint bucket, le filtrage de sous-chemin est appliqué côté client
  (`inf_hf_path_match`) dans `/hf list`, `/hf sync` et `/entrainement hf://…`.
- **`/page` « Device or resource busy » entre deux fetchs consécutifs** :
  le remount de `httpfs` sur `/page` fait désormais `settrans -g -f` puis
  jusqu'à 6 tentatives de `settrans -a` (sur Hurd, la suppression de
  l'ancien translator peut laguer d'un instant).
- **Écritures `/db` du client jamais traitées** : le translator data-base
  n'assemble une ligne que sur le `\n` final (`io_write`), et le client
  écrivait ses enregistrements sans — les inserts `training_data` étaient
  perdus et la lecture renvoyait le slot statut initial (compteurs à 0).
  Nouveau `write_root_line` (ajoute le `\n`) utilisé pour toutes les
  écritures `/db` (`/db status`, `/db count`, enregistrements
  d'entraînement). Les écritures `/llmN` restent sans `\n` (le
  neuron-translator traite chaque write entier).
- **Listings HF tronqués à 1000 entrées** : l'API tree pagine à 1000
  entrées par page avec un curseur `Link: rel="next"` — `/hf list`,
  `/hf sync` et `/entrainement hf://…` suivent désormais toutes les pages
  (`inf_hf_link_next` + `inf_hf_array_merge` ; tampon de lecture porté à
  4 Mo, une page fait ~200 Ko).
- **Narration des chemins absolus** (`/sigmoid`, `/entrainement` sur
  `/llmN`) : `narrate()` préfixait `g_mount` (`→ read /inference//llm1`) ;
  nouvelle helper `narrate_path()` pour les chemins hors montage.
- **DNS auto** : quand `getent` échoue et que `/etc/resolv.conf` n'a aucun
  `nameserver` (VM QEMU user-net), le client ajoute `nameserver 10.0.2.3`
  puis réessaie avant de renoncer (avec une indication explicite).
- **`/db count`** interroge aussi le vrai compteur `training_data` du
  translator `/db` (instruction `{"status": true}`), en plus du cache local.
- **`INF_JSON_MAX`** : dimensionné depuis `INF_PROMPT_MAX`
  (`12 * INF_PROMPT_MAX + 64`) au lieu d'un 8192 fixe — le JSON rendu dans
  `/inference/request` n'est plus tronqué (JSON invalide) quand le prompt
  est long.

### Modifié

- **Découpage en modules** (un module logique par fichier) :
  `src/client-core.c` → `client-core.c` + `training.c` + `text.c` +
  `stats.c` + `jsonlite.c` + `history.c` ; `src/client.c` → `client.c` +
  `nodeio.c` + `cmds.c` ; internes partagés dans `src/client_internal.h`.
  `src/Makefile.am` archive tout le cœur pur dans `libinfcore.a`
  (`contract.c expr.c actions.c spinner.c fs.c client-core.c training.c
  text.c stats.c jsonlite.c history.c`) ; les lignes de compilation
  manuelles de `SPEC.md` (section 9, 11, 12) ont été mises à jour en
  conséquence.

## [0.1.0] — 2026-11-20

Première version publique (GNU AI 0.1.0).

### Ajouté

- **Translator Hurd `/inference`** (`inference-translator` sur GNU/Hurd) :
  structure libnetfs canonique (`netfs_startup` + `netfs_server_loop`),
  nœuds POSIX du contrat v1 : `prompt`, `request`, `urls`, `task`,
  `params`, `status`, `version`. L'écriture de `prompt` déclenche
  l'analyse (extraction d'URLs, inférence de tâche) et la requête JSON
  se lit dans `request` ; `status` passe à `parsed`.
- **Client interactif `inference`** (POSIX pur, termios + ANSI, sans
  bibliothèque TUI) : `inference [--mount DIR]`, commandes `/editor`,
  `/task`, `/submit`, `/fetch`, `/status`, `/stats`, `/help`, `/quit`.
- **Édition de ligne** : Tab complète (commande ou tâche), DEL efface,
  `\` en tête de ligne échappe le `/`, flèches CSI gérées ; affichage
  corrigé en mode `cbreak` POSIX (plus d'« escalier »).
- **`/fetch URL`** : monte `httpfs` sur `/page` via `settrans` et affiche
  le statut HTTP ; en cas d'échec, affiche l'erreur de transport lue dans
  `/page/headers` (`httpfs-error: ...`) et l'état du translator.
- **`/stats`** : affichage temps réel CPU/mémoire/IO (une touche pour
  quitter) + barre d'état en une ligne au-dessus du prompt `>`. Sur Hurd,
  CPU via `host_statistics(HOST_CPU_LOAD_INFO)` et mémoire via
  `host_statistics64(HOST_VM_INFO64)` (les pages `inactive` de Mach sont
  comptées comme libres) ; IO en `n/d` (pas de `/proc/diskstats` sur
  Hurd).
- **Page de man `inference(1)`** (installée par `make install`), avec une
  section « UTILISER LE LLM (PILE GNU AI) ».
- **`hurd-vm/`** : driver `hurd_vm.py` (copie CI de l'image, réparation
  e2fsck, patch grub.cfg `console=com0`, login série, exécution scriptée
  dans le guest, options `--ram/--fresh/--keep/--root-pass/--timeout`),
  scripts `guest-netfix.sh`, `guest-inference.sh`, `inject-src.sh`,
  `launch_inference.py` (monte `/orchestrate`, `/inference`, `/web` puis
  lance le client), workflow CI `.github/workflows/hurd-vm.yml`.
- **Binaire portable** (hors Hurd) : `inference-translator --version`,
  `--help`, prototype `--cli` ncurses si détecté.

### Corrigé

- SIGSEGV de `/task` sans argument.
- Affichage en « escalier » du client (remplacement de `cfmakeraw` par
  un `cbreak` POSIX standard).
- `/stats` et la barre d'état affichent `n/d` quand les compteurs
  `/proc` sont absents ou vides (Hurd) au lieu d'un 0 trompeur.
- Mémoire affichée à ~100 % sur Hurd : les pages `inactive` de Mach sont
  désormais comptabilisées comme disponibles.
