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

## [Unreleased]

### Ajouté

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
  formateur pur `inf_format_uptime` testé (70/70, contrat 36/36 — 14
  commandes). Documentation à jour : `inference(1)` (options `--db`,
  `--orchestrate`, `--history`, commandes `/learn` `/read-code`
  `/learn-code` `/watch` `/history` `/uptime`, flèches haut/bas,
  variables `INFERENCE_DB`/`INFERENCE_ORCH`/`INFERENCE_HISTORY`).

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
