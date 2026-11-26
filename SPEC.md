<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Inference Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# SPEC — Contrats gelés de la phase 0

Ce document **gèle** les contrats de `inference-translator`. Les noms,
les schémas et les formats ci-dessous ne sont **pas renommables** :
`orchestrator-translator` (section 3.3 de son `PLAN.md`) et
`data-base-translator` (registre des clés SSH) en dépendent. Toute
évolution passe par un bump de `version` (section 7), jamais par un
changement silencieux.

Références : `PLAN.md` sections 3.3–3.6 (cadrage), issues #1–#6
(cartes phase 0 du `KANBAN.md`).

---

## 1. Le trio de requêtes JSON

Trois types de requêtes JSON, **identiques en local** (fichiers lus
et écrits) **et en distant** (trames du protocole SSH, section 7).
Rien d'autre ne circule entre l'utilisateur et le système.

### Type 1 — `request`

Le prompt traduit en requête structurée. En local : `write` sur
`/inference/prompt`, `read` de `/inference/request`. En distant :
trame `request` émise après le `hello`.

Schéma (l'ordre des clés est celui produit par
`inf_request_to_json ()`, à l'octet près) :

```json
{"prompt":"résume https://example.org/article en 5 points","task":"summarize","urls":["https://example.org/article"],"params":{}}
```

| Clé | Type | Description |
|---|---|---|
| `prompt` | chaîne | prompt brut, tel que soumis (échappement JSON standard, UTF-8 conservé) |
| `task` | chaîne | voir section 3 — `summarize` \| `translate` \| `answer` \| `classify` \| `custom` |
| `urls` | tableau de chaînes | URLs extraites, dans l'ordre d'apparition, ponctuation finale rognée |
| `params` | objet | paramètres additionnels ; **vide** en phase 0 (`{}`) |

### Type 2 — `status`

Suivi de l'exécution orchestrée. En local : `read` de
`/orchestrate/status` (et `/inference/status` pour l'état de
l'interface elle-même).

```json
{"run_id":42,"state":"running","instances":[{"id":1,"topology":"10,20,5","state":"busy"}]}
```

`state` de l'interface (`/inference/status`) : `idle` \| `composing`
\| `parsed` \| `submitted`.

### Type 3 — `result`

Réponse agrégée finale, seule sortie « officielle » du système. En
local : `read` de `/orchestrate/result`.

```json
{"run_id":42,"aggregate_strategy":"majority","output":"1. … 2. …","confidence":0.87}
```

---

## 2. Arborescence POSIX du translator `/inference`

```
/inference
├── prompt      (write)  prompt brut soumis par l'utilisateur
│               (read)   dernier prompt en cours
├── request     (read)   requête structurée (type 1, JSON)
├── urls        (read)   URLs extraites, une par ligne, dans l'ordre
├── task        (r/w)    summarize | translate | answer | classify | custom
├── params      (r/w)    paramètres additionnels (JSON, optionnel)
├── status      (read)   idle | composing | parsed | submitted
└── version     (read)   version et contrats supportés
```

L'interface n'écrit **jamais** dans `/orchestrate` : elle ne fait que
lire `status` (type 2) et `result` (type 3).

## 3. Extraction structurée (phase 0 : cœur pur, testé)

- **URL** : expression rationnelle POSIX (ERE) gelée :

  ```c
  #define INF_URL_REGEX "https?://[^[:space:]\"'<>]+"
  ```

  La ponctuation finale (`,.;:!?`) est rognée après extraction. Au
  plus `INF_URL_MAX` URLs par prompt.
- **Tâche** : heuristique par mots-clés, table gelée `inf_task_rules[]`
  (les accents sont en alternation `r(é|e)sum(é|e)` car une classe
  POSIX `[...]` travaille par octet et ne matcherait pas `é` en UTF-8) :

  | Tâche | Motifs (extraits) |
  |---|---|
  | `summarize` | `r(é|e)sum(é|e)`, `summar(y|ize)`, `summary` |
  | `translate` | `tradui(s|t)`, `translat` |
  | `answer` | `r(é|e)pond(s|re)`, `answer` |
  | `classify` | `class(e|ify)`, `cat(é|e)gor` |

  Sans correspondance : `task` vaut `custom` et l'orchestrateur applique
  sa politique par défaut. Surcharge manuelle via `write` sur
  `/inference/task` (phase 1).
- **Paramètres** : `params` reste `{}` en phase 0.

## 4. Points de montage

| Point de montage | Rôle |
|---|---|
| `/inference` | translator de l'interface (section 2) |
| `/orchestrate` | orchestrateur : `status` (type 2), `result` (type 3), lu en streaming |
| `/db` | registre PostgreSQL servi par `data-base-translator` (section 7) |

L'interface ne connaît que ces points de montage et les schémas de la
section 1 — jamais les binaires des autres translators (chacun reste
remplaçable).

## 5. Protocole éditeur

Ordre de résolution gelé : `$VISUAL` → `$EDITOR` → `nano` → `vi`.

- L'éditeur est lancé par `fork` + `exec` sur un **fichier temporaire**
  sous `/tmp`.
- **Code de sortie nul** : le buffer est relu et re-soumis (relecture
  avant soumission).
- Code de sortie non nul : le buffer est abandonné, l'interface reste
  sur le prompt précédent.
- Aucun éditeur ne nécessite de protocole dédié ; les éditeurs
  graphiques ne sont ni requis ni exclus.

## 6. Palette et conventions d'affichage

- Codes d'échappement ANSI (256 couleurs) écrits directement sur le
  terminal, `termios` pour le mode brut — **pas de bibliothèque TUI
  requise** : le binaire de base se compile avec la seule toolchain
  GNU/Hurd (`PLAN.md` §4).
- Thèmes de base : bannière, titres, texte, URL, commandes, erreurs,
  états. Le prototype `--cli` (section 8) passe par ncurses **si**
  `configure` le trouve (`HAVE_NCURSES`) ; sinon il est simplement
  absent et le cœur reste compilable.
- **`NO_COLOR`** : si la variable est définie et non vide, aucun code
  ANSI n'est émis.
- **Sortie non-TTY** (tube, fichier) : aucun code ANSI ne doit fuir ;
  le mode interactif refuse de démarrer sans terminal avec un message
  explicite.

## 7. Mode distant : SSH, trames, clés

- **Session** : `inference --remote utilisateur@hôte` exécute
  `ssh utilisateur@hôte inference-serveur`. Le processus serveur,
  lancé par la session, parle le protocole à trames sur les flux stdio
  du tunnel SSH et n'accède qu'aux fichiers `/inference` et
  `/orchestrate` du cluster. **Aucun port d'écoute** dédié.
- **Trame** : en-tête fixe (type : `hello` | `request` | `status` |
  `result` | `error`, longueur de la charge) suivi d'une charge JSON —
  exactement les objets de la section 1.
- **Poignée de main** : à l'ouverture, le client envoie
  `hello {"version":1}` ; le serveur répond `accept` (avec sa `version`)
  ou `error`. Aucun secret ne transite dans le protocole : l'identité
  est celle de la clé SSH authentifiée par sshd.
- **Version** : la `version` du contrat est négociée dans le `hello` ;
  un écart de version majeur produit `error`, pas un comportement
  silencieux.
- **Clés SSH nominatives** (ed25519 recommandé) :
  - la partie **privée** ne quitte jamais le poste de l'utilisateur ;
  - la partie **publique** est enregistrée par l'opérateur dans le
    registre `users` / `access_keys` servi par `data-base-translator`
    (`/db`) ;
  - le serveur génère `authorized_keys` **depuis ce registre** — seule
    source de vérité des accès distants ;
  - **révocation côté serveur** : révoquer une clé dans le registre puis
    re-générer `authorized_keys` coupe l'utilisateur sans toucher les
    autres, sans redéploiement ;
  - chaque échec d'authentification SSH est journalisé côté serveur
    (`auth_failures` : empreinte, origine, horodatage) via `/db`.
- Chiffrement, authentification et journalisation des tentatives sont
  délégués à OpenSSH : **zéro bibliothèque de chiffrement** liée au
  projet.

## 8. Le prototype CLI `--cli` (phase 0)

Le binaire accepte `--cli` (prototype d'interface, compilé seulement
si `HAVE_NCURSES`) :

- **Bannière** en haut (nom, version).
- **Fenêtre principale** : liste « Actions » (commandes slash gelées :
  `/editor`, `/task`, `/submit`, `/status`, `/quit`), liste
  « Expressions » (dont la regex URL gelée), narration des actions,
  dernière requête JSON affichée.
- **Ligne sablier** animée pendant la soumission ; `q` ou `Échap`
  interrompt.
- **Fenêtre de prompt** `>` en bas ; saisie UTF-8 (`setlocale`).
- Sans TTY, `--cli` échoue avec un message explicite (testé par
  `tests/test_cli.sh`).

### Le sablier (données pures, testées)

Carte 7×7, 10 images. Alphabet : `#` verre, `s` sable, `|` filet
(colonne du milieu), `.` vide. Le sable ne coule que vers le bas :
la première image a le haut plein, la dernière a le bas plein.
Table gelée : `inf_spinner[10][7][8]` dans `src/spinner.c`.

## 9. Construire et vérifier

```sh
# Cœur pur + tests de contrat (aucune dépendance, aucun TTY) :
gcc -std=c23 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Iinclude -o /tmp/test_contract \
    tests/test_contract.c src/contract.c src/expr.c src/actions.c src/spinner.c
/tmp/test_contract                 # 36/36 OK

# Build complet (autotools ; ncurses optionnel) :
./autogen.sh && ./configure && make && make check
sh tests/test_cli.sh src/inference-translator
./src/inference-translator --version
./src/inference-translator --cli   # prototype ncurses, TTY requis
```

## 10. Conventions de code

- C23 (`-std=c23`), POSIX.1-2008, style GNU (espace avant les
  parenthèses), `-Wall -Wextra`.
- En-tête SPDX `GPL-3.0-or-later` +
  `SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>`
  sur chaque fichier.
- Commentaires **en anglais** dans le code, documentation **en
  français**.
- Harnais de tests maison (macros `check` et compteurs), zéro
  framework externe — même convention que `neuron-translator`.
- Zéro allocation dans les chemins chauds : buffers fournis par
  l'appelant, tableaux de taille fixe (`INF_URL_MAX`, `INF_URL_LEN`).
- Erreurs réseau/SQL ≠ erreurs POSIX : un statut HTTP non-200 est une
  donnée affichée, jamais une erreur de l'interface.

## 11. Phase 1 — comportement du translator `/inference` (MVP)

Le translator est monté par `settrans -a /inference inference-translator`
(sur GNU/Hurd, build `ON_HURD` : `main-hurd.c` + `netfs-hooks.c`, moteur
pur dans `src/fs.c`). L'arborescence gelée (section 2) est statique :
un répertoire racine et sept nœuds fixes.

### Écritures

| Nœud | Effet |
|---|---|
| `prompt` (w) | stocke le prompt brut, le parse en requête type 1 (`request`), statut → `parsed`. La tâche est re-détectée, sauf si `/inference/task` a été surchargé manuellement. |
| `task` (w) | surcharge **collante** de la tâche (`summarize` \| `translate` \| `answer` \| `classify` \| `custom`) ; la dernière `request` est mise à jour. Nom invalide → refus. Écrire `auto` efface la surcharge et re-détecte la tâche depuis le prompt courant. |

### Lectures

| Nœud | Contenu |
|---|---|
| `prompt` (r) | dernier prompt brut soumis |
| `request` (r) | JSON type 1 exact de la dernière requête ; illisible (`ENODATA`) avant tout prompt |
| `urls` (r) | URLs extraites, une par ligne, dans l'ordre ; idem avant tout prompt |
| `task` (r) | tâche effective (surcharge ou détection) |
| `params` (r) | `{}` en phase 1 |
| `status` (r) | `idle` \| `composing` \| `parsed` \| `submitted` (`parsed` après un `write` sur `prompt`) |
| `version` (r) | `{"program":"inference-translator","version":"0.1.0","contract":1}` |

### Erreurs

Nœud inconnu → refus ; nœud en lecture seule en écriture → refus ;
`request`/`urls` avant tout prompt → `ENODATA`. Aucun état partiel
n'est jamais visible. Le serveur est mono-thread (`netfs_server_loop`)
: les RPCs sont sérialisées, l'état du moteur n'a pas besoin de
verrou en phase 1.

### Vérification

```sh
# Cœur + tests, partout (aucune dépendance Hurd) :
gcc -std=c23 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Iinclude -o /tmp/test_fs tests/test_fs.c \
    src/contract.c src/expr.c src/actions.c src/spinner.c src/fs.c
/tmp/test_fs                        # 29/29 OK
./autogen.sh && ./configure && make && make check   # + test_posix.sh sur Hurd monté
# Sur GNU/Hurd, translator réellement monté :
settrans -a /inference inference-translator
echo "résume https://example.org/article" | tee /inference/prompt
cat /inference/request              # JSON type 1 exact
cat /inference/status               # parsed
```

## 12. Phase 2 — le client `inference` (IHM minimale)

Nouveau binaire `inference` (build portable, aucune bibliothèque TUI :
termios brut + codes ANSI). Il ne parle qu'au point de montage, par
`open`/`read`/`write` POSIX : `/inference` par défaut, surchargeable
par `--mount DIR` ou `$INFERENCE_MOUNT` (utilisé par les tests avec un
faux arbre).

- **Bannière** colorée (nom, version, contrat), `NO_COLOR` respecté
  (section 6) ; sans TTY sur stdin **et** stdout, le mode interactif
  refuse de démarrer avec un message explicite (exit 2).
- **Saisie multi-lignes** en mode brut `termios` : chaque ligne non
  commande est ajoutée au buffer du prompt ; `Entrée` termine une
  ligne, `Retour arrière` efface, `Ctrl-C`/`Ctrl-D` (ligne vide)
  quittent.
- **Commandes slash** (table gelée `inf_actions[]`) :
  - `/editor` : écrit le buffer dans un fichier temporaire
    `/tmp/inf-editor-XXXXXX`, lance l'éditeur (`$VISUAL` → `$EDITOR` →
    `nano` → `vi`, section 5) par `fork`+`exec` ; code de sortie nul →
    le fichier est relu dans le buffer (relecture avant re-soumission)
    ; code non nul → buffer inchangé ;
  - `/task TÂCHE` : valide le nom (`summarize|translate|answer|classify|custom`)
    puis écrit `<mount>/task` ;
  - `/submit` : écrit le buffer dans `<mount>/prompt`, puis lit
    `<mount>/request` (JSON type 1 affiché) et `<mount>/status` ;
  - `/status` : lit `<mount>/status` ;
  - `/quit` : quitte (exit 0) ;
  - `/reply [question]` : répond selon les données d'entraînement du cache
    local NDJSON (`$INFERENCE_TRAINING` ou `$HOME/.inference-training.ndjson`,
    rempli par `/learn`, `/learn-code`, `/entrainement`) — recherche par
    mots-clés (`inf_training_search`) ;
  - `/sigmoid` : visualise l'état des neurones montés `/llm1..N` — sorties
    courantes du neuron-translator passées par la sigmoïde, une barre ASCII
    par neurone de sortie (`inf_parse_neuron_output`, `inf_sigmoid_render`) ;
  - `/entrainement URL|hf://… [dir]` : avec une page http(s), la télécharge,
    la découpe en extraits, fait passer chaque extrait par le
    neuron-translator (`input` sur `/llm1..N`, lecture de la sortie) et
    enregistre {entrée, sortie} dans `/db` (`training_data`) et le cache
    local ; avec une URL `hf://` (`inf_hf_url`), télécharge d'abord tous les
    fichiers du dépôt HF dans `[dir]` (défaut `./data`, comme
    `hf sync ./data hf://…`) puis applique le même passage par neurone ;
  - `/hf list|sync hf://[buckets|datasets|models]/<owner>/<repo>[/prefix] [dir]` :
    liste l'arbre d'un dépôt Hugging Face via l'API HTTP
    (`inf_hf_api_url`) ou le synchronise vers un dossier local
    (`/hf sync hf://buckets/... ./local` : téléchargement par l'endpoint
    `resolve` `inf_hf_resolve_url`, copie locale, puis conversion de chaque
    fichier en enregistrements `/db` `training_data` — découpage, SHA-256,
    `inf_training_record` — et cache local) ; les buckets (`hf://buckets/…`)
    sont sans révision : l'arbre se liste via `…/api/buckets/<o>/<r>/tree?recursive=true`
    (sans `/main`, qui répond `[]`) et le téléchargement via
    `…/buckets/<o>/<r>/resolve/<path>` (sans `/main`, qui 404) ; le `prefix`
    étant ignoré côté serveur pour les buckets, le filtrage de sous-chemin est
    appliqué côté client (`inf_hf_path_match`) ; l'API tree pagine à 1000
    entrées par page, donc `/hf list`, `/hf sync` et `/entrainement hf://…`
    suivent le curseur `Link: rel="next"` (`inf_hf_link_next`) et fusionnent
    les pages en un seul tableau (`inf_hf_array_merge`) ;
  - `/db status|search MOT|list [N]|count` : exploration de la base —
    `status` écrit `{"status": true}` sur `/db` et affiche la réponse ;
    `search`/`list`/`count` parcourent le cache local (miroir NDJSON de
    `training_data`, `inf_training_search_list`).
  - `/code [langage] description` : premier bloc de code fenced (```)
    non trivial parmi les 20 meilleurs résultats de la recherche
    (`inf_training_search_list`, triée par score IDF) — d'abord sur le
    texte brut du chunk, puis après décodage (`decode_training_text`) ;
    coloration syntaxique + URL source affichées ; repli : texte décodé
    du meilleur résultat, plafonné à 2000 caractères ;
  - **Données Python depuis Hugging Face** : `/hf sync
    hf://datasets/<owner>/<repo>[/prefix] <dossier-local>` télécharge
    (endpoint `resolve`) puis convertit chaque fichier en enregistrements
    `/db` `training_data`. Jeux conseillés pour la VM (non gated, taille
    raisonnable) :
    - `hf://datasets/ise-uiuc/Magicoder-OSS-Instruct-75K`
      (`data-oss_instruct-decontaminated.jsonl`, ~203 Mo, problèmes/solutions
      riches en Python) ;
    - `hf://buckets/clairemariecoralie/DeepSeek-V4.1-Flash-Nemotron-Competitive-Coding-Distill-bucket`
      (`data/train.jsonl`) ;
    - éviter `bigcode/the-stack-smol` (gated) et le bucket `cyber-security`
      (~95 Go). Ensuite : `/db search python`, `/code python …` (les mots
      rares pèsent plus grâce au score IDF).
    Note : les gros fichiers LFS redirigent vers le CDN
    (`cdn-lfs.hf.co`, `cas-bridge.xethub.hf.co`, `transfer.xethub.hf.co`) —
    épingler ces hôtes dans `/etc/hosts` si le DNS slirp est cassé.
- **DNS automatique** : à chaque URL ajoutée (`/fetch`, `/learn`,
  `/read-code`, `/learn-code`, `/entrainement`), si l'hôte n'est pas une IP
  littérale et absent de `/etc/hosts`, il est résolu (`getent hosts`) puis
  ajouté à `/etc/hosts` — httpfs atteint ainsi la cible sans DNS fonctionnel
  dans la VM (`inf_url_host`, `inf_hosts_has`).
- **Ligne d'information** : à chaque prompt, une ligne atténuée
  (`inf_client_stats_line` : CPU %, mémoire) est affichée entre la ligne de
  prompt et le reste de la fenêtre.
- **Narration** : chaque étape affiche une ligne `→ <verbe> <mount>/<nœud>`
  nommant le nœud POSIX concerné (ex. `→ write /inference/prompt`).

### Vérification

```sh
gcc -std=c23 -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -Iinclude -o /tmp/test_client tests/test_client.c \
    src/contract.c src/expr.c src/actions.c src/spinner.c src/fs.c src/client-core.c \
    src/training.c src/text.c src/stats.c src/jsonlite.c src/history.c src/neuron.c -lm
/tmp/test_client                    # cœur pur du client (112/112)
./autogen.sh && ./configure && make && make check   # + test_client.sh (pty via script(1))
sh tests/test_client.sh src/inference
# Sur GNU/Hurd avec le translator monté, en interactif (console série) :
inference            # puis /editor, /submit, /status, /quit
```
