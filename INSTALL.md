<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright (C) 2026 Claire Ivanenka <claire@gnu-ai.org>

This file is part of the Inference Translator and is free software:
you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.
-->

# INSTALL — compiler et installer `inference-translator`

`inference-translator` est un projet autotools en C23. Sur **GNU/Hurd**
il produit le translator POSIX monté sur `/inference` ; sur tout autre
hôte il produit un binaire de vérification portable plus le client
interactif `inference`.

## 1. Prérequis

Commun : un compilateur C23 (`gcc` récent), `make`, autoconf, automake,
libtool.

- **Debian GNU/Linux** :
  ```sh
  sudo apt install build-essential autoconf automake libtool libncurses-dev
  ```
  (`libncurses-dev` est optionnel : sans lui, le prototype `--cli` est
  simplement absent.)
- **Debian GNU/Hurd** (dans la VM, voir `hurd-vm/README.md`) :
  ```sh
  apt update
  apt install build-essential gcc hurd-dev gnumach-dev git make autoconf automake libtool
  ```
  Note : sur hurd-amd64, `hurd-dev` installe `idvec.h` dans
  `/usr/include/x86_64-gnu/` et non `/usr/include/hurd/` ; si la
  compilation échoue sur `hurd/idvec.h`, faire :
  ```sh
  ln -sf /usr/include/x86_64-gnu/idvec.h /usr/include/hurd/idvec.h
  ```

## 2. Compilation

```sh
./autogen.sh                 # ou : autoreconf -i
./configure                  # ajoutez --prefix=/usr si besoin (défaut /usr/local)
make
make check                   # suite de tests (test_contract, test_fs, test_cli, test_posix)
sudo make install            # binaires + page de man
```

Ou en une fois, installation automatique :

```sh
./scripts/install.sh                 # installe les dépendances si root, compile, teste, installe
./scripts/install.sh --no-check      # sans la suite de tests
./scripts/install.sh --prefix=/usr   # change le préfixe d'installation
./scripts/install.sh --help
```

## 3. Ce qui est installé

| Fichier | Rôle |
|---|---|
| `/usr/local/bin/inference-translator` | Sur Hurd : le translator `/inference`. Ailleurs : binaire de vérification (`--version`, `--help`, `--cli`). |
| `/usr/local/bin/inference` | Client interactif (`inference --mount /inference`, voir `/help` dans le client et `man inference`). |
| `/usr/local/share/man/man1/inference.1` | Page de man `inference(1)`. |

## 4. Utilisation sur GNU/Hurd

```sh
mkdir -p /inference
settrans -a /inference /usr/local/bin/inference-translator

echo "résume https://example.org/article" > /inference/prompt
cat /inference/request        # requête JSON du contrat v1
cat /inference/status         # parsed

inference --mount /inference  # client interactif (/help, /submit, /fetch URL, /stats, /quit)
```

La pile complète (orchestrateur, neurones, httpfs) se monte avec
`hurd-vm/launch_inference.py` — voir `hurd-vm/README.md`.

## 5. Test dans une VM GNU/Hurd (QEMU)

Voir `hurd-vm/README.md` : image cdimage, `hurd_vm.py`, injection des
sources (`inject-src.sh`) ou clone réseau, puis `guest-inference.sh`.

## 6. Désinstallation

```sh
sudo make uninstall
```
