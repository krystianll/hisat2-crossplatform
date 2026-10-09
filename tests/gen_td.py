"""Deterministic test data for the hisat2 launcher parity tests.

Writes into the directory given as argv[1]:
  ref.fa           two random chromosomes
  se.fq / se.fq.gz single-end: aligning reads (lengths 100 and 75) + random reads
  m1.fq / m2.fq    paired: concordant pairs (--fr) + random pairs
  se.fa            FASTA reads (for -f)
"""
import gzip, os, random, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rnd = random.Random(7)
comp = str.maketrans("ACGT", "TGCA")


def rc(s):
    return s.translate(comp)[::-1]


chroms = {name: "".join(rnd.choice("ACGT") for _ in range(6000)) for name in ("chrA", "chrB")}
with open(os.path.join(out, "ref.fa"), "w", newline="\n") as f:
    for name, seq in chroms.items():
        f.write(f">{name}\n")
        for i in range(0, len(seq), 60):
            f.write(seq[i:i + 60] + "\n")


def sample(length):
    seq = chroms[rnd.choice(sorted(chroms))]
    p = rnd.randint(0, len(seq) - length)
    s = seq[p:p + length]
    return s if rnd.random() < 0.5 else rc(s)


def rand_seq(length):
    return "".join(rnd.choice("ACGT") for _ in range(length))


se = []
for i in range(120):
    se.append((f"a{i}", sample(100 if i % 3 else 75)))
for i in range(40):
    se.append((f"r{i}", rand_seq(100)))
with open(os.path.join(out, "se.fq"), "w", newline="\n") as f:
    for n, s in se:
        f.write(f"@{n}\n{s}\n+\n{'I' * len(s)}\n")
with open(os.path.join(out, "se.fq"), "rb") as src, gzip.open(os.path.join(out, "se.fq.gz"), "wb") as dst:
    dst.write(src.read())
with open(os.path.join(out, "se.fa"), "w", newline="\n") as f:
    for n, s in se:
        f.write(f">{n}\n{s}\n")

with open(os.path.join(out, "m1.fq"), "w", newline="\n") as f1, \
        open(os.path.join(out, "m2.fq"), "w", newline="\n") as f2:
    for i in range(80):
        if i < 60:
            name = sorted(chroms)[i % 2]
            seq = chroms[name]
            p = rnd.randint(0, len(seq) - 300)
            frag = seq[p:p + 300]
            a, b = frag[:100], rc(frag[-100:])
        else:
            a, b = rand_seq(100), rand_seq(100)
        f1.write(f"@p{i}/1\n{a}\n+\n{'I' * 100}\n")
        f2.write(f"@p{i}/2\n{b}\n+\n{'I' * 100}\n")
print("test data in", out)
