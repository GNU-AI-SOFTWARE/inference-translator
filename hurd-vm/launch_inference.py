#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Claire Ivanenka <claire@gnu-ai.org>
"""Boot the whole GNU AI translator stack on GNU/Hurd, then drop into
the interactive `inference` client.

Run as root inside the VM (e.g. over `ssh -p 2222 root@127.0.0.1`):

    python3 /root/launch_inference.py                 # full stack
    python3 /root/launch_inference.py --no-db --no-web
    python3 /root/launch_inference.py --dry-run       # print the plan only

The project checkouts are expected under --src-root (default /root):

    inference-translator/  orchestrator-translator/  data-base-translator/
    neuron-translator/     httpfs-translator/

(clone them in the guest, or inject pre-built tarballs with
inject-src.sh from mistral-vm-debian-hurd).  Each tree is built with
autoreconf/configure/make when its binary is missing.

Mounts, in dependency order (stale mounts are dropped first):

    /db           db-translator --conninfo "dbname=gnuai"
                  (the PostgreSQL cluster is started and the role/db are
                   created when pg_ctlcluster is present; without
                   PostgreSQL, /orchestrate falls back to --storage mem)
    /orchestrate  orchestrator-translator --storage db:/db
                   --instances node:/llm --neuron <sigmoid-neuron-translator>
                  (the scheduler mounts the /llm<N> nodes itself)
    /inference    inference-translator
    /web          httpfs <URL>            (skipped with --no-web)

Then execs: inference --mount /inference   (the phase-2 interactive
client: /editor, /task, /submit, /status, /quit).  If the client
binary is unavailable, a minimal Python REPL over the /inference nodes
is used instead.
"""

import argparse
import os
import shutil
import subprocess
import sys
import time


DRY = False


def run(cmd, cwd=None, check=True):
    print("+ " + " ".join(cmd) + (f"   (in {cwd})" if cwd else ""))
    if DRY:
        return subprocess.CompletedProcess(cmd, 0)
    return subprocess.run(cmd, cwd=cwd, check=check)


def build_tree(src_root, name, binary_rel, dry_run):
    """Return the absolute path of the tree's binary, building the
    tree first when the binary is missing."""
    tree = os.path.join(src_root, name)
    binary = os.path.join(tree, binary_rel)
    if dry_run or os.path.exists(binary):
        return binary
    if not os.path.isdir(tree):
        sys.exit(f"missing checkout: {tree} (clone it or inject a tarball)")
    if not os.path.exists(os.path.join(tree, "configure")):
        run(["autoreconf", "-i"], cwd=tree)
    run(["./configure"], cwd=tree)
    run(["make"], cwd=tree)
    if not os.path.exists(binary):
        sys.exit(f"the build of {tree} produced no {binary_rel}")
    return binary


def drop(mount_point, dry_run):
    if not dry_run:
        subprocess.run(["settrans", "-g", mount_point], capture_output=True)


def mount(mount_point, prep, cmd, dry_run, settle=0):
    drop(mount_point, dry_run)
    if not dry_run:
        prep(mount_point)
    run(["settrans", "-a", mount_point] + cmd, check=not dry_run)
    if settle and not dry_run:
        time.sleep(settle)


def prep_dir(mp):
    os.makedirs(mp, exist_ok=True)


def prep_node(mp):
    subprocess.run(["rm", "-rf", mp], capture_output=True)
    open(mp, "w").close()


def setup_postgres(dry_run):
    """Start the local cluster and create the role/db that db-translator
    connects to (same recipe as orchestrator-translator/ci/guest-hurd.sh:
    Hurd has no reliable peer credentials, so local connections are
    trusted).  Returns True when `psql -d gnuai` works as root."""
    if shutil.which("pg_ctlcluster") is None or shutil.which("psql") is None:
        return False
    if dry_run:
        return True
    versions = sorted(os.listdir("/etc/postgresql")) \
        if os.path.isdir("/etc/postgresql") else []
    if not versions:
        return False
    ver = versions[-1]
    run(["pg_ctlcluster", ver, "main", "start"], check=False)
    hba = f"/etc/postgresql/{ver}/main/pg_hba.conf"
    with open(hba, "w") as f:
        f.write("local   all   all                 trust\n"
                "host    all   all   127.0.0.1/32  trust\n"
                "host    all   all   ::1/128       trust\n")
    run(["pg_ctlcluster", ver, "main", "restart"], check=False)
    run(["psql", "-U", "postgres", "-tAc",
         "create role root login superuser"], check=False)
    run(["dropdb", "-U", "postgres", "--if-exists", "gnuai"], check=False)
    run(["createdb", "-U", "postgres", "-O", "root", "gnuai"], check=False)
    r = subprocess.run(["psql", "-d", "gnuai", "-tAc", "select current_user"],
                       capture_output=True, text=True)
    ok = r.returncode == 0 and r.stdout.strip() == "root"
    print(f"[{'ok' if ok else 'warn'}] PostgreSQL gnuai as root: "
          f"{r.stdout.strip() or r.stderr.strip()[:200]}")
    return ok


def repl(mount_point):
    """Minimal fallback client over the /inference nodes."""
    print("inference REPL (fallback): type a prompt, /quit to exit")
    while True:
        try:
            line = input("> ")
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if line.strip() in ("/quit", "/q", "exit", "quit"):
            break
        if not line.strip():
            continue
        with open(os.path.join(mount_point, "prompt"), "w") as f:
            f.write(line + "\n")
        for node in ("request", "status", "urls"):
            try:
                with open(os.path.join(mount_point, node)) as f:
                    print(f"--- {node} ---")
                    print(f.read())
            except OSError as e:
                print(f"({node}: {e})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src-root", default="/root",
                    help="directory holding the project checkouts "
                         "(default: /root)")
    ap.add_argument("--url", default="https://example.org",
                    help="base URL mounted at /web by httpfs")
    ap.add_argument("--no-db", action="store_true",
                    help="do not mount /db (orchestrator uses --storage mem)")
    ap.add_argument("--no-web", action="store_true",
                    help="do not mount /web")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan without executing anything")
    opts = ap.parse_args()
    global DRY
    DRY = opts.dry_run

    if os.geteuid() != 0 and not opts.dry_run:
        sys.exit("must run as root inside the Hurd VM "
                 "(mounting translators needs root)")
    if shutil.which("settrans") is None and not opts.dry_run:
        sys.exit("settrans not found: this script runs inside the "
                 "GNU/Hurd guest, not on the host")

    src = opts.src_root
    inf_bin = build_tree(src, "inference-translator",
                         "src/inference-translator", opts.dry_run)
    cli_bin = os.path.join(src, "inference-translator", "src", "inference")
    orch_bin = build_tree(src, "orchestrator-translator",
                          "src/orchestrator-translator", opts.dry_run)
    neuron_bin = build_tree(src, "neuron-translator",
                            "src/sigmoid-neuron-translator", opts.dry_run)
    db_bin = None
    if not opts.no_db:
        db_bin = build_tree(src, "data-base-translator",
                            "src/db-translator", opts.dry_run)
    httpfs_bin = None
    if not opts.no_web:
        httpfs_bin = build_tree(src, "httpfs-translator",
                                "src/httpfs", opts.dry_run)

    use_db = db_bin is not None and setup_postgres(opts.dry_run)
    if db_bin is not None and not use_db:
        print("[warn] no usable PostgreSQL: /orchestrate will use "
              "--storage mem (test-only backend, never in production)")

    if use_db:
        mount("/db", prep_node, [db_bin, "--conninfo", "dbname=gnuai"],
              opts.dry_run, settle=2)
    # Stale instance nodes from a previous orchestrator run.
    for n in range(1, 9):
        drop(f"/llm{n}", opts.dry_run)
    orch_cmd = [orch_bin, "--storage", "db:/db" if use_db else "mem",
                "--instances", "node:/llm", "--neuron", neuron_bin]
    if httpfs_bin is not None:
        # Phase 3 of the orchestrator ({"command":"infer"}): it mounts
        # httpfs itself per URL, on /tmp/orc-fetch/<id>.
        orch_cmd += ["--httpfs", httpfs_bin, "--fetch", "node:/tmp/orc-fetch"]
    mount("/orchestrate", prep_node, orch_cmd, opts.dry_run, settle=2)
    mount("/inference", prep_dir, [inf_bin], opts.dry_run, settle=1)
    if httpfs_bin is not None:
        mount("/web", prep_dir, [httpfs_bin, opts.url], opts.dry_run)

    if opts.dry_run:
        return 0

    for node in ("/orchestrate", "/inference/status"):
        try:
            with open(node) as f:
                print(f"--- {node} ---")
                print(f.read())
        except OSError:
            pass

    print("stack mounted; launching the interactive client "
          "(/editor /task /submit /status /quit)")
    if os.path.exists(cli_bin):
        os.execv(cli_bin, ["inference", "--mount", "/inference"])
    repl("/inference")
    return 0


if __name__ == "__main__":
    sys.exit(main())
