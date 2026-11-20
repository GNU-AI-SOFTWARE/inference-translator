<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Inference Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# INSTALL — building and installing `inference-translator`

`inference-translator` is an autotools C23 project. On **GNU/Hurd** it
builds the POSIX translator mounted on `/inference`; on any other host
it builds a portable verification binary plus the interactive `inference`
client.

(French version: `INSTALL.md`.)

## 1. Prerequisites

Common: a C23 compiler (recent `gcc`), `make`, autoconf, automake,
libtool.

- **Debian GNU/Linux**:
  ```sh
  sudo apt install build-essential autoconf automake libtool libncurses-dev
  ```
  (`libncurses-dev` is optional: without it the `--cli` prototype is
  simply not built.)
- **Debian GNU/Hurd** (inside the VM, see `hurd-vm/README.md`):
  ```sh
  apt update
  apt install build-essential gcc hurd-dev gnumach-dev git make autoconf automake libtool
  ```
  Note: on hurd-amd64, `hurd-dev` installs `idvec.h` under
  `/usr/include/x86_64-gnu/`, not `/usr/include/hurd/`; if compilation
  fails on `hurd/idvec.h`, run:
  ```sh
  ln -sf /usr/include/x86_64-gnu/idvec.h /usr/include/hurd/idvec.h
  ```

## 2. Build

```sh
./autogen.sh                 # or: autoreconf -i
./configure                  # add --prefix=/usr if needed (default /usr/local)
make
make check                   # test suite (test_contract, test_fs, test_cli, test_posix)
sudo make install            # binaries + man page
```

Or all at once, automatic installation:

```sh
./install.sh                 # installs dependencies if root, builds, tests, installs
./install.sh --no-check      # skip the test suite
./install.sh --prefix=/usr   # change the install prefix
./install.sh --help
```

## 3. What gets installed

| File | Role |
|---|---|
| `/usr/local/bin/inference-translator` | On Hurd: the `/inference` translator. Elsewhere: verification binary (`--version`, `--help`, `--cli`). |
| `/usr/local/bin/inference` | Interactive client (`inference --mount /inference`, see `/help` in the client and `man inference`). |
| `/usr/local/share/man/man1/inference.1` | `inference(1)` man page. |

## 4. Usage on GNU/Hurd

```sh
mkdir -p /inference
settrans -a /inference /usr/local/bin/inference-translator

echo "summarize https://example.org/article" > /inference/prompt
cat /inference/request        # contract v1 JSON request
cat /inference/status         # parsed

inference --mount /inference  # interactive client (/help, /submit, /fetch URL, /stats, /quit)
```

The full stack (orchestrator, neurons, httpfs) is mounted by
`hurd-vm/launch_inference.py` — see `hurd-vm/README.md`.

## 5. Testing in a GNU/Hurd VM (QEMU)

See `hurd-vm/README.md`: cdimage, `hurd_vm.py`, source injection
(`inject-src.sh`) or network clone, then `guest-inference.sh`.

## 6. Uninstall

```sh
sudo make uninstall
```
