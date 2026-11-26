/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * actions.c -- Frozen slash-command table (SPEC.md section 8).
 *
 * Pure data: the CLI prototype lists inf_actions[] in the "Actions"
 * pane of the main window.  Commands are user-facing (French), code
 * comments stay English.
 */

#include "inference.h"

const struct inf_action inf_actions[] =
{
    { "/editor", "composer le prompt dans $VISUAL/$EDITOR" },
    { "/task",   "forcer le type de tâche (…, auto = détection auto)" },
    { "/submit", "analyser le prompt et soumettre la requête" },
    { "/fetch",  "télécharger une page HTML via httpfs (montée sur /page)" },
    { "/status", "afficher l'état de l'interface" },
    { "/stats",  "CPU/mémoire/IO en temps réel (une touche pour quitter)" },
    { "/help",   "afficher cette aide" },
    { "/quit",   "quitter" },
    { "/learn",  "enregistrer les URL de la requête comme éléments d'entraînement dans /db (training_data)" },
    { "/read-code", "lire le code d'une URL : langage détecté + coloration syntaxique" },
    { "/learn-code", "lire le code d'une URL (langage détecté) ET l'enregistrer dans /db (training_data)" },
    { "/uptime", "version du client et du translator + uptime de la session" },
    { "/history", "lister l'historique de la session (flèche haut/bas pour rappeler)" },
    { "/watch", "surveiller /orchestrate : status (instances) puis résultat agrégé en streaming" },
    { "/reply", "répondre depuis les données d'entraînement via le neurone /llm1..N si monté, sinon le cache" },
    { "/sigmoid", "visualiser l'état des neurones montés (/llmN) après sigmoïde" },
    { "/entrainement", "remplir /db avec les données d'une URL ou hf://…, passées par le neuron-translator" },
    { "/hf", "hf://buckets/... : lister (list) ou synchroniser (sync) un bucket/dataset HF vers un dossier local, puis ingérer dans /db" },
    { "/code", "code depuis /db : /code [langage] description (ex. /code c hello world)" },
    { "/db", "base en source de vérité : /db status|search MOT|list [N]|count lisent /db d'abord, /db sync recopie /db dans le cache" },
};

const size_t inf_actions_count =
    sizeof inf_actions / sizeof inf_actions[0];
