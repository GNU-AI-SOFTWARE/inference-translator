# Test sur VM GNU/Hurd (QEMU)

Scripts pour tester `inference-translator` sur un vrai GNU/Hurd via
[mistral-vm-debian-hurd](https://github.com/GNU-AI-SOFTWARE/mistral-vm-debian-hurd)
(`hurd_vm.py`, image `debian-hurd-amd64-20260314.img` du cdimage).

## Procédure (hôte)

```console
$ git clone https://github.com/GNU-AI-SOFTWARE/mistral-vm-debian-hurd
$ cd mistral-vm-debian-hurd
$ wget https://cdimage.debian.org/cdimage/ports/latest/hurd-amd64/debian-hurd-amd64-20260314.img.tar.xz
$ tar xJf debian-hurd-amd64-20260314.img.tar.xz
$ sudo apt install qemu-system-x86 e2fsprogs python3 autoconf automake libtool git

# copier les scripts depuis cette branche (pas de heredoc) :
$ for f in guest-inference.sh guest-netfix.sh inject-src.sh; do
>   curl -LO "https://raw.githubusercontent.com/GNU-AI-SOFTWARE/inference-translator/vibe/hurd-vm-scripts-807684/hurd-vm/$f"
> done
$ chmod +x guest-*.sh inject-src.sh

# 1) diagnostic/réparation réseau du guest (optionnel) :
$ python3 hurd_vm.py debian-hurd-amd64-20260314.img guest-netfix.sh --ram 2G
# attendu : DNS-OK et HTTP-OK (eth0 10.0.2.15/24, gw 10.0.2.2, DNS 10.0.2.3)

# 2) test 100% offline de inference-translator (recommandé) :
$ git clone https://github.com/GNU-AI-SOFTWARE/inference-translator ~/inference-translator
$ cd ~/inference-translator && autoreconf -i && cd ~/mistral-vm-debian-hurd
$ tar czf ~/inference-src.tar.gz -C ~ inference-translator
$ ./inject-src.sh debian-hurd-amd64-20260314.img ~/inference-src.tar.gz
$ python3 hurd_vm.py debian-hurd-amd64-20260314.img guest-inference.sh --ram 2G
```

Attendu : `test_contract` 36/36, `test_fs` 28/28, `test_cli.sh` OK,
`test_posix.sh` SKIP 77 avant montage puis OK, `cat /inference/request`
= JSON exact du contrat, `status` = `parsed`, override `task` pris en
compte, `GUEST-INF-OK`, `GUESTRC=0`, exit code 0 du driver, puis
`guest powered off cleanly`.

## Pousser tout ça dans `mistral-vm-debian-hurd` (depuis ta machine)

Le bac à sable ne peut signer des commits que pour `inference-translator`,
donc le push vers `mistral-vm-debian-hurd` se fait depuis ton poste (ta
propre signature).  Le patch complet contre `main` est fourni ici :
`hurd-vm/mistral-vm-debian-hurd.patch` (scripts + README + `hurd_vm.py`
patché « arrêt propre »).

```console
$ cd ~/mistral-vm-debian-hurd
$ curl -Lo /tmp/mvmdh.patch "https://raw.githubusercontent.com/GNU-AI-SOFTWARE/inference-translator/vibe/hurd-vm-scripts-807684/hurd-vm/mistral-vm-debian-hurd.patch"
$ git checkout -b vibe/vm-guest-scripts-807684
$ git apply --check /tmp/mvmdh.patch && git apply /tmp/mvmdh.patch
$ git add -A
$ git commit -m "Add guest scripts + always-clean-poweroff driver for Hurd VM tests"
$ git push -u origin vibe/vm-guest-scripts-807684
$ gh pr create --repo GNU-AI-SOFTWARE/mistral-vm-debian-hurd --draft \
    --head vibe/vm-guest-scripts-807684 --base main \
    --title "Guest scripts + clean poweroff driver for Hurd VM tests" \
    --body "guest-netfix.sh (static pfinet on slirp net), guest-inference.sh
(inference-translator build+mount test, offline via inject-src.sh),
inject-src.sh (debugfs injection into the CI image), and a hurd_vm.py
patch that always sends poweroff and waits for the ACPI exit, so a
failed guest script no longer leaves the CI copy dirty (no more
e2fsck repair on the next run)."
```

## Arrêt propre de la VM (patch de `hurd_vm.py`)

`hurd_vm.py` amont n'envoie `poweroff` que si le script guest a réussi
(rc=0) : en cas d'échec il tue QEMU, la copie CI reste « dirty » et le
run suivant refait un `dd` + `e2fsck` complet avant de booter.  La
copie patchée `hurd-vm/hurd_vm.py` de cette branche envoie toujours
`poweroff` (même après un échec) et attend la sortie ACPI de QEMU :
la copie CI reste « clean », plus de réparation au run suivant.

Installation locale (le bac à sable ne peut pas pousser dans
`mistral-vm-debian-hurd` — la signature de commits y est refusée ;
ouvrir la PR amont depuis ta propre machine) :

```console
$ cd ~/mistral-vm-debian-hurd
$ cp hurd_vm.py hurd_vm.py.orig
$ curl -Lo hurd_vm.py "https://raw.githubusercontent.com/GNU-AI-SOFTWARE/inference-translator/vibe/hurd-vm-scripts-807684/hurd-vm/hurd_vm.py"
$ python3 -m py_compile hurd_vm.py
```

Ensuite tous les runs, succès ou échec du script guest, se terminent
par `[vm] guest powered off cleanly` et la copie CI reste « clean ».

## SSH dans la VM

Le port hôte 2222 n'existe que tant que QEMU tourne : le driver
connecte root lui-même sur la console série, exécute le script guest,
puis éteint la VM — l'accès SSH « disparaît » donc à chaque fin de run
(c'est normal, `Connection refused` = aucune VM active).  Pour une
session interactive : injecter sa clé publique dans
`/root/.ssh/authorized_keys` de la copie CI une fois pour toutes via
un petit script guest, faire terminer le script guest par
`sleep 3600`, puis, pendant que le driver tourne, depuis un second
terminal de l'hôte :
`ssh -i ~/.ssh/id_ed25519 -p 2222 root@127.0.0.1`.
Autre option : lancer QEMU à la main sur `debian-hurd-ci.img` (mêmes
flags que `hurd_vm.py`, `-serial file:serial-live.log`), la VM reste
alive tant que tu veux.  Note : `inject-src.sh` exige qu'aucune VM ne
tourne sur l'image (lock QEMU).
