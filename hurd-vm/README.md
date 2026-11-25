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

# 1) réseau du guest : intégré à guest-inference.sh (guest_net : ifup/DHCP
#    via netdde d'abord (l'image bind 10.0.2.15 au boot), puis si échec
#    (ifstate périmé) ifconfig statique + route par défaut, puis dhclient -1,
#    et pfinet en dernier recours sur un /servers/socket/2 libre seulement ;
#    resolv.conf DNS 10.0.2.3 + retries TCP, épinglage /etc/hosts de secours
#    deb.debian.org / cdimage.debian.org / huggingface.co /
#    datasets-server.huggingface.co / github.com).  Les adresses viennent des
#    HURD_VM_GUEST_* exportées par le driver (HURD_VM_NET côté hôte, ex.
#    10.106.74.0/24 -> guest 10.106.74.15) ; en mode bridge HURD_VM_TAP,
#    HURD_VM_MODE=tap configure l'IP LAN en statique.
#    guest-netfix.sh reste disponible pour le diagnostic manuel :
#    python3 hurd_vm.py debian-hurd-amd64-20260314.img guest-netfix.sh --ram 2G
# attendu : DNS-OK et HTTP-OK

# 2) test 100% offline de inference-translator (recommandé) :
$ git clone https://github.com/GNU-AI-SOFTWARE/inference-translator ~/inference-translator
$ cd ~/inference-translator && autoreconf -i && cd ~/mistral-vm-debian-hurd
#   autoreconf -i MUST succeed (automake >= 1.17): the guest image has no
#   autotools and guest-inference.sh now refuses an incomplete generated
#   build system (GUEST-BUILD-KO) instead of dying in make
$ tar czf ~/inference-src.tar.gz -C ~ inference-translator
$ ./inject-src.sh debian-hurd-amd64-20260314.img ~/inference-src.tar.gz
$ python3 hurd_vm.py debian-hurd-amd64-20260314.img guest-inference.sh --ram 2G
```

Attendu : `make check` 6/6 PASS — `test_cli.sh`, `test_client.sh` (fallback
ptyrun : pas de `script(1)` sur l'image Hurd), `test_contract` 36/36,
`test_fs` 34/34, `test_client` 120/120, `test_posix.sh` OK (`/inference`
monté par le script avant `make check`) ; puis `cat /inference/request`
= JSON exact du contrat, `status` = `parsed`, override `task` pris en
compte, `GUEST-INF-OK`, `GUESTRC=0`, exit code 0 du driver, puis
`guest powered off cleanly`.

## SSH dans la VM

Le port hôte 2222 n'existe que tant que QEMU tourne.  Avec `--keep`,
le driver laisse QEMU tourner (session détachée) et démarre le sshd
du guest avec un self-check (listeners IPv4 `:22`, bannières), donc
après le run :

```console
$ python3 hurd_vm.py debian-hurd-amd64-20260314.img guest-inference.sh \
      --ram 2G --keep --set-root-pass gnu-ai --ssh-key ~/.ssh/hurd_vm_ed25519.pub
$ ssh -i ~/.ssh/hurd_vm_ed25519 -o IdentitiesOnly=yes -o PasswordAuthentication=no \
      -p 2222 root@127.0.0.1
```

`--ssh-key` injecte la clé publique dans `/root/.ssh/authorized_keys`
de la copie CI (comme `--set-root-pass` fixe le mot de passe root) ;
passer les **mêmes flags à chaque run** réutilise la copie CI telle quelle.
`HURD_VM_SSH_BIND=<ip hôte>` ajoute un second forward sur cette IP (LAN) ;
`HURD_VM_NET=<sous-réseau>` change le réseau slirp (guest = <net>.15) ;
`HURD_VM_TAP=<iface>` passe en bridge (IP LAN réelle pour la VM).
Voir le README de mistral-vm-debian-hurd pour le détail.

## Console interactive : `launch_inference.py`

Une fois root en SSH dans la VM, avec les cinq dépôts du projet sous
`/root` (`inference-translator/ orchestrator-translator/
data-base-translator/ neuron-translator/ httpfs-translator/`) :

```console
root@hurd:~# python3 /root/launch_inference.py
```

Le script construit chaque arbre si le binaire manque, monte dans
l'ordre `/db` (démarre le cluster PostgreSQL local et crée le rôle
`root` + la base `gnuai` si `pg_ctlcluster` est présent), puis
`/orchestrate` (`--storage db:/db --instances node:/llm --neuron
.../sigmoid-neuron-translator` ; l'orchestrateur monte lui-même les
nœuds `/llm<N>`), puis `/inference`, puis `/web` (httpfs), et lance
enfin le client interactif `inference --mount /inference`
(`/editor`, `/task`, `/submit`, `/status`, `/quit`).  Options :
`--src-root DIR`, `--url URL`, `--no-db`, `--no-web`, `--dry-run`
(affiche le plan sans rien exécuter).  Sans PostgreSQL disponible,
`/orchestrate` retombe sur `--storage mem` (backend de test uniquement).

## CI GitHub (`.github/workflows/hurd-vm.yml`)

Le workflow `hurd-vm` rejoue exactement cette procédure sur un runner
`ubuntu-latest` (pas de `/dev/kvm` → QEMU en TCG, plus lent ;
`timeout-minutes: 60`) : checkout du dépôt + de
`mistral-vm-debian-hurd`, image cdimage, tarball de sources avec
`configure` pré-généré, `inject-src.sh`, puis
`hurd_vm.py debian-hurd-amd64-20260314.img guest-inference.sh --ram 2G`.
Le `serial.log` est uploadé comme artifact à chaque run.  Couvre
l'issue #27 (CI sous QEMU).
