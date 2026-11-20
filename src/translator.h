/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * translator.h -- internal glue between the portable core (fs.c) and
 * the GNU/Hurd netfs server (netfs-hooks.c, main-hurd.c).  Only the
 * Hurd build compiles those two files.
 */

#ifndef INF_TRANSLATOR_H
#define INF_TRANSLATOR_H

#include "inference.h"

/* libnetfs's struct node carries an opaque `struct netnode *nn`;
 * ours only needs the node name and whether the node accepts writes
 * (the frozen tree is static, SPEC.md section 2). */
struct netnode
{
    const char *name;           /* node name under /inference, "/" for root */
    int writable;
};

/* Create the root node and the frozen /inference child nodes, and
 * publish the root as netfs_root_node.  Called by main-hurd.c after
 * netfs_init () and inf_fs_init (). */
void inf_translator_init (struct inf_fs *fs);

#endif /* INF_TRANSLATOR_H */
