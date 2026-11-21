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
    { "/task",   "forcer le type de tâche (summarize, translate, ...)" },
    { "/submit", "analyser le prompt et soumettre la requête" },
    { "/fetch",  "télécharger une page HTML via httpfs (montée sur /page)" },
    { "/status", "afficher l'état de l'interface" },
    { "/stats",  "CPU/mémoire/IO en temps réel (une touche pour quitter)" },
    { "/help",   "afficher cette aide" },
    { "/quit",   "quitter" },
    { "/learn",  "enregistrer les URL de la requête comme éléments d'entraînement dans /db (training_data)" },
    { "/read-code", "lire le code d'une URL : langage détecté + coloration syntaxique" },
    { "/learn-code", "lire le code d'une URL (langage détecté) ET l'enregistrer dans /db (training_data)" },
};

const size_t inf_actions_count =
    sizeof inf_actions / sizeof inf_actions[0];
