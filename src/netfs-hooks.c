/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org> */

/*
 * netfs-hooks.c -- libnetfs callbacks of the /inference translator
 * (phase 1 MVP, GNU/Hurd only; compiled when ON_HURD).
 *
 * The frozen tree (SPEC.md section 2) is static: one root directory
 * and seven fixed nodes (prompt, request, urls, task, params, status,
 * version).  All node semantics live in the portable core (fs.c);
 * this file only maps the Hurd RPCs onto it — the same split as
 * data-base-translator (core + trivfs-hooks.c).
 *
 * The server loop is single threaded (netfs_server_loop), so the
 * engine state needs no lock in the MVP.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if ON_HURD == 1

/* GNU Mach 1.8+git20260224 (the 2026-03-14 preinstalled image): its
 * installed headers drop the processor_name_array_t typedef, so the
 * MIG-generated mach_host.h - and with it <mach.h> - does not
 * compile.  The current ports package carries the typedef in
 * <mach/mach_types.h> as `typedef mach_port_t *processor_name_array_t;`.
 * Define it the SAME way here: C tolerates the identical
 * redefinition, so this keeps compiling on fixed systems. */

#include <mach/port.h>                  /* mach_port_t */
typedef mach_port_t *processor_name_array_t;

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <hurd/netfs.h>
#include <hurd/iohelp.h>

#include "translator.h"

#ifndef VERSION
#define VERSION "0.1.0"
#endif

/* --- Variables required by libnetfs ------------------------------- */

char *netfs_server_name = "inference-translator";
char *netfs_server_version = VERSION;
int netfs_maxsymlinks = 0;

/* --- The static tree ---------------------------------------------- */

static struct inf_fs *g_fs;
static struct netnode g_root_nn = { "/", 0 };
static struct node *g_root;
static struct netnode g_child_nn[8];
static struct node *g_children[8];

void
inf_translator_init (struct inf_fs *fs)
{
    g_fs = fs;
    size_t n = inf_fs_node_count ();
    for (size_t i = 0; i < n && i < 8; i++)
      {
        const char *name = inf_fs_nodes ()[i];
        g_child_nn[i].name = name;
        g_child_nn[i].writable =
            (strcmp (name, INF_NODE_PROMPT) == 0
             || strcmp (name, INF_NODE_TASK) == 0);
        g_children[i] = netfs_make_node (&g_child_nn[i]);
      }
    g_root = netfs_make_node (&g_root_nn);
    netfs_root_node = g_root;
}

/* fnv1a -- 64-bit FNV-1a hash, used for stable inode numbers. */
static uint64_t
fnv1a (const char *s)
{
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *) s; *p != '\0'; p++)
      {
        h ^= (uint64_t) *p;
        h *= 1099511628211ULL;
      }
    return h;
}

/* --- libnetfs callbacks ------------------------------------------- */

error_t
netfs_attempt_lookup (struct iouser *user, struct node *dir,
                      const char *name, struct node **np)
{
    (void) user;
    *np = NULL;

    if (strcmp (name, ".") == 0)
      {
        netfs_nref (dir);
        *np = dir;                      /* dir stays locked, per contract */
        return 0;
      }

    if (strcmp (name, "..") == 0)
      {
        struct node *root = g_root;
        netfs_nref (root);
        pthread_mutex_unlock (&dir->lock);
        pthread_mutex_lock (&root->lock);
        *np = root;
        return 0;
      }

    for (size_t i = 0; i < inf_fs_node_count (); i++)
        if (strcmp (name, g_child_nn[i].name) == 0)
          {
            struct node *hit = g_children[i];
            netfs_nref (hit);
            pthread_mutex_unlock (&dir->lock);
            pthread_mutex_lock (&hit->lock);
            *np = hit;
            return 0;
          }

    pthread_mutex_unlock (&dir->lock);
    return ENOENT;
}

/* Directory entry packing, same convention as the other Hurd
 * translators (cf. httpfs-translator). */
#define DIRENT_ALIGN     4
#define DIRENT_NAME_OFFS offsetof (struct dirent, d_name)
#define DIRENT_LEN(namelen)                                                \
    ((DIRENT_NAME_OFFS + (namelen) + 1 + (DIRENT_ALIGN - 1))               \
     & ~((size_t) (DIRENT_ALIGN - 1)))
#define DIRENTS_SIZE     (8 * 1024)

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

error_t
netfs_get_dirents (struct iouser *cred, struct node *dir, int entry,
                   int nentries, char **data,
                   mach_msg_type_number_t *datacnt, vm_size_t bufsize,
                   int *amt)
{
    (void) cred;
    (void) bufsize;

    *amt = 0;
    *datacnt = 0;

    if (dir->nn != &g_root_nn)
      {
        pthread_mutex_unlock (&dir->lock);
        return ENOTDIR;
      }

    int total = 2 + (int) inf_fs_node_count ();     /* "." ".." + nodes */
    if (entry >= total || nentries == 0)
      {
        pthread_mutex_unlock (&dir->lock);
        return 0;
      }

    int nb = total - entry;
    if (nentries != -1 && nentries < nb)
        nb = nentries;

    /* netfs_S_dir_readdir sets data_dealloc = 1: the buffer MUST come
     * from mmap (VM memory), never malloc. */
    char *start = mmap (NULL, DIRENTS_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (start == MAP_FAILED)
      {
        pthread_mutex_unlock (&dir->lock);
        return ENOMEM;
      }

    char *p = start;
    for (int i = entry; i < entry + nb; i++)
      {
        const char *name = (i == 0) ? "."
                           : (i == 1) ? ".."
                           : g_child_nn[i - 2].name;
        size_t name_len = strlen (name);
        size_t reclen = DIRENT_LEN (name_len);

        struct dirent hdr;
        memset (&hdr, 0, sizeof hdr);
#ifdef _DIRENT_HAVE_D_NAMLEN
        hdr.d_namlen = name_len;        /* Hurd's dirent carries it */
#else
        (void) name_len;
#endif
        hdr.d_fileno = (ino_t) fnv1a (name);
        hdr.d_reclen = reclen;
        hdr.d_type = (i < 2) ? DT_DIR : DT_REG;

        memcpy (p, &hdr, DIRENT_NAME_OFFS);
        strcpy (p + DIRENT_NAME_OFFS, name);
        p += reclen;
      }

    /* Return the unused tail of the region, page-aligned (ftpfs
     * discipline, required by data_dealloc = 1). */
    long page = sysconf (_SC_PAGESIZE);
    uintptr_t end = ((uintptr_t) p + (uintptr_t) page - 1)
                    & ~((uintptr_t) page - 1);
    if (end < (uintptr_t) (start + DIRENTS_SIZE))
        munmap ((void *) end,
                (size_t) ((uintptr_t) (start + DIRENTS_SIZE) - end));

    *data = start;
    *amt = nb;
    *datacnt = (mach_msg_type_number_t) (p - start);
    pthread_mutex_unlock (&dir->lock);
    return 0;
}

error_t
netfs_validate_stat (struct node *np, struct iouser *cred)
{
    (void) cred;
    struct netnode *nn = np->nn;

    memset (&np->nn_stat, 0, sizeof np->nn_stat);
    np->nn_stat.st_ino = (ino_t) fnv1a (nn->name);
    np->nn_stat.st_blksize = 4096;

    if (nn == &g_root_nn)
      {
        np->nn_stat.st_mode = S_IFDIR | 0555;
        np->nn_stat.st_nlink = 2 + (int) inf_fs_node_count ();
        np->nn_stat.st_size = 4096;     /* indicative */
      }
    else
      {
        ssize_t sz = inf_fs_read (g_fs, nn->name, NULL, 0);
        if (sz < 0)
            sz = 0;                     /* not parsed yet: empty */
        np->nn_stat.st_mode = S_IFREG | (nn->writable ? 0644 : 0444);
        np->nn_stat.st_nlink = 1;
        np->nn_stat.st_size = (off_t) sz;
      }

    np->nn_stat.st_blocks = (np->nn_stat.st_size + 511) / 512;
    np->nn_translated = np->nn_stat.st_mode;
    return 0;
}

error_t
netfs_attempt_read (struct iouser *cred, struct node *np, loff_t offset,
                    size_t *len, void *data)
{
    (void) cred;
    struct netnode *nn = np->nn;

    if (nn == &g_root_nn)
        return EISDIR;

    ssize_t total = inf_fs_read (g_fs, nn->name, NULL, 0);
    if (total < 0)
        return ENODATA;                 /* e.g. request before any prompt */

    if (offset < 0)
        offset = 0;
    if ((uint64_t) offset >= (uint64_t) total)
      {
        *len = 0;
        return 0;                       /* EOF */
      }

    size_t want = *len;
    if ((uint64_t) offset + want > (uint64_t) total)
        want = (size_t) (total - offset);

    /* Render the node once, then serve the caller's window. */
    char *tmp = malloc ((size_t) total + 1);
    if (tmp == NULL)
        return ENOMEM;
    inf_fs_read (g_fs, nn->name, tmp, (size_t) total + 1);
    memcpy (data, tmp + offset, want);
    free (tmp);

    *len = want;
    return 0;
}

error_t
netfs_attempt_write (struct iouser *cred, struct node *np, loff_t offset,
                     size_t *len, const void *data)
{
    (void) cred;
    (void) offset;                      /* whole-write semantics */
    struct netnode *nn = np->nn;

    if (nn == &g_root_nn)
        return EISDIR;
    if (!nn->writable)
        return EROFS;

    if (inf_fs_write (g_fs, nn->name, (const char *) data, *len) != 0)
        return EINVAL;
    return 0;                           /* *len unchanged: all consumed */
}

/* A truncate (the O_TRUNC of `tee` / `>`) arrives as set_size: the
 * nodes are protocol endpoints, not mutable files — accept it and
 * change nothing (same rule as data-base-translator). */
error_t
netfs_attempt_set_size (struct iouser *cred, struct node *np, loff_t size)
{
    (void) cred;
    (void) np;
    (void) size;
    return 0;
}

error_t
netfs_attempt_statfs (struct iouser *cred, struct node *np,
                      fsys_statfsbuf_t *st)
{
    (void) cred;
    (void) np;
    memset (st, 0, sizeof *st);
    st->f_bsize = 4096;
    st->f_blocks = 1024;
    st->f_bfree = 1024;
    return 0;
}

error_t
netfs_report_access (struct iouser *cred, struct node *np, int *types)
{
    (void) cred;
    struct netnode *nn = np->nn;
    *types = O_RDONLY | (nn->writable ? O_WRONLY : 0);
    return 0;
}

error_t
netfs_check_open_permissions (struct iouser *user, struct node *np,
                              int flags, int newnode)
{
    (void) user;
    (void) np;
    (void) flags;
    (void) newnode;
    return 0;                           /* no access control in the MVP */
}

/* The tree is static: the nodes live for the whole mount. */
void
netfs_node_norefs (struct node *np)
{
    (void) np;
}

void
netfs_try_dropping_softrefs (struct node *np)
{
    (void) np;
}

struct iouser *
netfs_make_user (uid_t *uids, int nuids, uid_t *gids, int ngids)
{
    struct iouser *user = NULL;
    if (iohelp_create_complex_iouser (&user, uids, nuids, gids, ngids) != 0)
        return NULL;
    return user;
}

/* --- Mutation callbacks: the tree is frozen, refuse them all ------ */

error_t
netfs_attempt_chown (struct iouser *cred, struct node *np, uid_t uid,
                     uid_t gid)
{
    (void) cred; (void) np; (void) uid; (void) gid;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_chauthor (struct iouser *cred, struct node *np, uid_t author)
{
    (void) cred; (void) np; (void) author;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_chmod (struct iouser *cred, struct node *np, mode_t mode)
{
    (void) cred; (void) np; (void) mode;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_chflags (struct iouser *cred, struct node *np, int flags)
{
    (void) cred; (void) np; (void) flags;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_utimes (struct iouser *cred, struct node *np,
                      struct timespec *atime, struct timespec *mtime)
{
    (void) cred; (void) np; (void) atime; (void) mtime;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_mksymlink (struct iouser *cred, struct node *np,
                         const char *name)
{
    (void) cred; (void) np; (void) name;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_mkdev (struct iouser *cred, struct node *np, mode_t type,
                     dev_t indexes)
{
    (void) cred; (void) np; (void) type; (void) indexes;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_unlink (struct iouser *user, struct node *dir,
                      const char *name)
{
    (void) user; (void) dir; (void) name;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_rename (struct iouser *user, struct node *fromdir,
                      const char *fromname, struct node *todir,
                      const char *toname, int excl)
{
    (void) user; (void) fromdir; (void) fromname;
    (void) todir; (void) toname; (void) excl;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_mkdir (struct iouser *user, struct node *dir,
                     const char *name, mode_t mode)
{
    (void) user; (void) dir; (void) name; (void) mode;
    return EROFS;
}

error_t
netfs_attempt_rmdir (struct iouser *user, struct node *dir,
                     const char *name)
{
    (void) user; (void) dir; (void) name;
    return EROFS;
}

error_t
netfs_attempt_link (struct iouser *user, struct node *dir,
                    struct node *file, const char *name, int excl)
{
    (void) user; (void) dir; (void) file; (void) name; (void) excl;
    return EOPNOTSUPP;
}

error_t
netfs_attempt_mkfile (struct iouser *user, struct node *dir, mode_t mode,
                      struct node **np)
{
    (void) user; (void) dir; (void) mode; (void) np;
    return EROFS;
}

error_t
netfs_attempt_create_file (struct iouser *user, struct node *dir,
                           const char *name, mode_t mode, struct node **np)
{
    (void) user; (void) dir; (void) name; (void) mode; (void) np;
    return EROFS;
}

error_t
netfs_attempt_readlink (struct iouser *user, struct node *np, char *buf)
{
    (void) user; (void) np; (void) buf;
    return EINVAL;
}

error_t
netfs_attempt_sync (struct iouser *cred, struct node *np, int wait)
{
    (void) cred;
    (void) np;
    (void) wait;
    return 0;                           /* nothing to sync */
}

error_t
netfs_attempt_syncfs (struct iouser *cred, int wait)
{
    (void) cred;
    (void) wait;
    return 0;
}

#endif /* ON_HURD == 1 */
