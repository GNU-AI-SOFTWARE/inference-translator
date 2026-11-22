#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# train-from-db.py -- external trainer for neuron-translator.
#
# neuron-translator is an inference engine by design (frozen v1.0
# contract: no weight ever changes after init).  The missing piece is
# the external trainer its README specifies: it consumes the training
# data archived in /db (training_data, written by /learn, /learn-code,
# /entrainement, /hf sync or direct `tee /db`) and emits a .nn file in
# the documented v2 format, loadable with:
#
#     echo 'load /tmp/model.nn' | sudo tee /llm1
#
# Usage:
#   python3 scripts/train-from-db.py --out /tmp/model.nn
#   python3 scripts/train-from-db.py --cache ~/.inference-training.ndjson \
#       --topology 10,20,5 --epochs 200 --out /tmp/model.nn
#
# Sources, in order: psql on the base behind /db (--db gnuai), then a
# local NDJSON cache (--cache).  Records are either /entrainement pairs
# {"input": "...", "output": [...]} or conversations ("human: ..." /
# "gpt: ..." turns) split into (question -> answer) pairs.  The input
# vector is the exact byte histogram of inf_text_to_vector
# (inference-translator src/neuron.c), so a loaded neuron reproduces the
# trained mapping on `input`.

import argparse
import json
import math
import random
import struct
import subprocess
import sys

RESET_POTENTIAL = -80.0          # neuron.h RESET_POTENTIAL
NET_FILE_MAGIC = 0x4E455552     # 'N' 'E' 'U' 'R'
NET_FILE_VERSION = 2


def sigmoid(x):
    if x >= 0:
        e = math.exp(-x)
        return 1.0 / (1.0 + e)
    e = math.exp(x)
    return e / (1.0 + e)


def text_to_vec(text, n):
    """Exact replica of inf_text_to_vector (inference-translator
    src/neuron.c): 256 byte buckets spread evenly over the n outputs,
    each normalized by the total byte count."""
    buckets = [0] * 256
    data = text.encode('utf-8', 'replace')
    for b in data:
        buckets[b] += 1
    total = len(data)
    out = []
    for i in range(n):
        lo = (i * 256) // n
        hi = ((i + 1) * 256) // n
        if hi <= lo:
            hi = lo + 1
        acc = sum(buckets[lo:min(hi, 256)])
        out.append(acc / total if total > 0 else 0.0)
    return out


def pairs_from_record(rec, n_in, n_out):
    """Extract (input_vec, output_vec) training pairs from one
    training_data record (a JSON line, possibly wrapped in
    {"table":..., "row": {...}})."""
    row = rec.get('row', rec)
    content = row.get('content')
    if not content:
        return []
    try:
        c = json.loads(content)
    except (ValueError, TypeError):
        c = content
    pairs = []
    if isinstance(c, dict) and 'input' in c and 'output' in c:
        out = c['output']
        if isinstance(out, list) and len(out) == n_out:
            pairs.append((text_to_vec(str(c['input']), n_in),
                          [float(x) for x in out]))
        return pairs
    if isinstance(c, str) and ('human:' in c or 'gpt:' in c
                              or 'user:' in c or 'assistant:' in c):
        turns = []
        for line in c.splitlines():
            for role in ('human:', 'user:', 'gpt:', 'assistant:'):
                if line.startswith(role):
                    turns.append((role, line[len(role):].strip()))
                    break
        for (r1, q), (r2, a) in zip(turns, turns[1:]):
            if r1 in ('human:', 'user:') and r2 in ('gpt:', 'assistant:'):
                pairs.append((text_to_vec(q, n_in), text_to_vec(a, n_out)))
    return pairs


def load_records(args, n_in, n_out):
    lines = []
    try:
        r = subprocess.run(
            ['psql', '-d', args.db, '-tA', '-c',
             'select row_to_json(t) from training_data t order by id'],
            capture_output=True, text=True, timeout=120)
        if r.returncode == 0:
            lines = [l for l in r.stdout.splitlines() if l.startswith('{')]
    except (OSError, subprocess.TimeoutExpired):
        pass
    src = 'psql /db (%s)' % args.db
    if not lines and args.cache:
        with open(args.cache, encoding='utf-8') as f:
            lines = [l.strip() for l in f if l.strip().startswith('{')]
        src = 'cache %s' % args.cache
    pairs = []
    for line in lines[:args.max_records]:
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        pairs.extend(pairs_from_record(rec, n_in, n_out))
    print('# %d records -> %d training pairs (source: %s)'
          % (min(len(lines), args.max_records), len(pairs), src),
          file=sys.stderr)
    return pairs


class MLP:
    """Sigmoid feedforward net, same forward pass as neuron-translator
    (network_forward: sigmoid on every layer, bias + w.v per neuron)."""

    def __init__(self, sizes):
        self.sizes = sizes
        lc = len(sizes)
        tw = sum(sizes[i] * sizes[i - 1] for i in range(1, lc))
        # flat deterministic init, identical to network_init (neuron.c)
        wflat = []
        for i in range(tw):
            seed = (i * 2654435761) & 0xFFFFFFFF
            wflat.append((seed & 0x7FFFFF) / 0x7FFFFF * 0.4 - 0.2)
        self.W = []
        k = 0
        for l in range(1, lc):
            prev, curr = sizes[l - 1], sizes[l]
            mat = []
            for n in range(curr):
                mat.append(wflat[k:k + prev])
                k += prev
            self.W.append(mat)
        self.B = [[0.0] * sizes[l] for l in range(1, lc)]

    def forward(self, x):
        acts = [list(x)]
        for l in range(1, len(self.sizes)):
            prev = acts[-1]
            curr = []
            for n in range(self.sizes[l]):
                s = self.B[l - 1][n]
                row = self.W[l - 1][n]
                for p in range(len(prev)):
                    s += prev[p] * row[p]
                curr.append(sigmoid(s))
            acts.append(curr)
        return acts

    def train(self, pairs, epochs, lr, seed=1234):
        rng = random.Random(seed)
        L = len(self.sizes) - 1
        for ep in range(epochs):
            rng.shuffle(pairs)
            loss = 0.0
            for x, t in pairs:
                acts = self.forward(x)
                out = acts[L]
                delta = [(out[n] - t[n]) * out[n] * (1.0 - out[n])
                         for n in range(self.sizes[L])]
                deltas = [None] * L
                deltas[L - 1] = delta
                for l in range(L - 1, 0, -1):
                    nxt = deltas[l]
                    a = acts[l]
                    d = []
                    for p in range(self.sizes[l]):
                        s = 0.0
                        for n in range(self.sizes[l + 1]):
                            s += self.W[l][n][p] * nxt[n]
                        d.append(s * a[p] * (1.0 - a[p]))
                    deltas[l - 1] = d
                for l in range(1, L + 1):
                    for n in range(self.sizes[l]):
                        dn = deltas[l - 1][n]
                        row = self.W[l - 1][n]
                        for p in range(self.sizes[l - 1]):
                            row[p] -= lr * dn * acts[l - 1][p]
                        self.B[l - 1][n] -= lr * dn
                loss += sum((out[n] - t[n]) ** 2 for n in range(len(t)))
            if ep % max(1, epochs // 10) == 0 or ep == epochs - 1:
                print('# epoch %d/%d  mse=%.6f'
                      % (ep + 1, epochs, loss / max(1, len(pairs))),
                      file=sys.stderr)


def write_nn(path, sizes, mlp):
    """Serialize in the .nn v2 layout of network_save_fd (neuron.c):
    magic, version, 32-byte NetworkTopology, totals, offsets, then
    voltages/weights/biases as float32."""
    lc = len(sizes)
    tn = sum(sizes)
    tw = sum(sizes[i] * sizes[i - 1] for i in range(1, lc))
    tb = sum(sizes[1:])
    lo = [0]
    for i in range(1, lc):
        lo.append(lo[-1] + sizes[i - 1])
    wo = [0]
    for i in range(1, lc - 1):
        wo.append(wo[-1] + sizes[i] * sizes[i - 1])
    bo = [0]
    for i in range(1, lc):
        bo.append(bo[-1] + sizes[i])
    out = bytearray()
    out += struct.pack('<I H', NET_FILE_MAGIC, NET_FILE_VERSION)
    topo = (struct.pack('<B', lc) + b'\x00'
            + struct.pack('<8H', *(sizes + [0] * (8 - lc)))
            + struct.pack('<HH', sizes[0], sizes[-1])
            + struct.pack('<f', RESET_POTENTIAL) + b'\x00' * 6)
    assert len(topo) == 32
    out += topo
    out += struct.pack('<QQQ', tn, tw, tb)
    out += struct.pack('<%dQ' % lc, *lo)
    if lc > 1:
        out += struct.pack('<%dQ' % (lc - 1), *wo)
    out += struct.pack('<%dQ' % lc, *bo)
    out += struct.pack('<%df' % tn, *([RESET_POTENTIAL] * tn))
    wf = []
    for l in range(1, lc):
        for n in range(sizes[l]):
            wf.extend(mlp.W[l - 1][n])
    bf = []
    for l in range(1, lc):
        bf.extend(mlp.B[l - 1])
    out += struct.pack('<%df' % tw, *wf)
    out += struct.pack('<%df' % tb, *bf)
    with open(path, 'wb') as f:
        f.write(out)
    print('# wrote %s (%d bytes, topology %s, %d weights)'
          % (path, len(out), ','.join(map(str, sizes)), tw), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description='train a .nn from /db training_data')
    ap.add_argument('--db', default='gnuai', help='PostgreSQL db behind /db')
    ap.add_argument('--cache', default=None, help='local NDJSON cache fallback')
    ap.add_argument('--out', default='/tmp/model.nn')
    ap.add_argument('--topology', default='10,20,5')
    ap.add_argument('--epochs', type=int, default=100)
    ap.add_argument('--lr', type=float, default=0.5)
    ap.add_argument('--max-records', type=int, default=100000)
    ap.add_argument('--check', action='store_true',
                    help='print the forward pass of the first pair after training')
    args = ap.parse_args()

    sizes = [int(s) for s in args.topology.split(',')]
    if len(sizes) < 2 or len(sizes) > 8:
        sys.exit('topology must have 2..8 layers')
    pairs = load_records(args, sizes[0], sizes[-1])
    if not pairs:
        sys.exit('no training pairs found (check /db or --cache)')
    mlp = MLP(sizes)
    mlp.train(pairs, args.epochs, args.lr)
    write_nn(args.out, sizes, mlp)
    if args.check:
        x, t = pairs[0]
        y = mlp.forward(x)[-1]
        print('# check: target=%s' % ['%.4f' % v for v in t])
        print('# check: output=%s' % ['%.4f' % v for v in y])


if __name__ == '__main__':
    main()
