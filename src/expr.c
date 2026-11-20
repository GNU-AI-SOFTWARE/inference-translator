/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * expr.c -- Frozen expression and task-rule tables (SPEC.md section 3).
 *
 * Pure data: the CLI prototype shows inf_exprs[] in its "Expressions"
 * pane, and contract.c matches inf_task_rules[] to detect the task.
 *
 * Accents are written as alternations (r(é|e)sum(é|e)) because a POSIX
 * bracket expression [...] works byte per byte and would not match the
 * two-byte UTF-8 encoding of 'é'.
 */

#include "inference.h"

/* Expressions shown in the main window; the URL pattern is the frozen
 * extraction regex of SPEC.md section 3. */
const struct inf_expr inf_exprs[] =
{
    { "url", INF_URL_REGEX },
};

const size_t inf_exprs_count = sizeof inf_exprs / sizeof inf_exprs[0];

/* Task detection rules, tried in order; first match wins, default is
 * INF_TASK_CUSTOM (SPEC.md section 3). */
const struct inf_task_rule inf_task_rules[] =
{
    { INF_TASK_SUMMARIZE, "r(é|e)sum(é|e)|summar(y|ize)|summary" },
    { INF_TASK_TRANSLATE, "tradui(s|t)|translat" },
    { INF_TASK_ANSWER,    "r(é|e)pond(s|re)|answer" },
    { INF_TASK_CLASSIFY,  "class(e|ify)|cat(é|e)gor" },
};

const size_t inf_task_rules_count =
    sizeof inf_task_rules / sizeof inf_task_rules[0];
