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
