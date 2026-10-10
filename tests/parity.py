"""Compare the C launchers (hisat2_launcher.c) with the upstream Perl/Python
wrapper scripts. tests/run_parity.sh sets up the directories; see tests/README.md.

usage: python3 parity.py REF_DIR NEW_DIR DATA_DIR WORK_DIR [case-substring]
       python3 parity.py dump TOOL_DIR DATA_DIR WORK_DIR OUT.json [case-substring]
       python3 parity.py cmp A.json B.json      (e.g. Windows vs macOS launcher)

Each case runs the same command line through REF_DIR/<tool> and NEW_DIR/<tool>,
each in its own empty working directory, and compares exit code, stdout,
stderr and every file written there. Directory paths are normalised; gzip/bzip2
outputs are compared by compression type plus decompressed content. Exit
status 1 means a difference that is not in EXPECTED / EXPECTED_XPLAT.
"""
import bz2, difflib, gzip, json, os, shutil, subprocess, sys

MODE = sys.argv[1] if sys.argv[1] in ("dump", "cmp") else "compare"
if MODE == "compare":
    REF, NEW, TD, WORK = (os.path.abspath(p) for p in sys.argv[1:5])
    ONLY = sys.argv[5] if len(sys.argv) > 5 else ""
elif MODE == "dump":
    NEW, TD, WORK = (os.path.abspath(p) for p in sys.argv[2:5])
    OUT = sys.argv[5]
    ONLY = sys.argv[6] if len(sys.argv) > 6 else ""

CASES = [
    # (name, tool, args, stdin file or None)
    ("align-noargs", "hisat2", [], None),
    ("align-help", "hisat2", ["-h"], None),
    ("align-version", "hisat2", ["--version"], None),
    ("se-plain", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "-S", "out.sam"], None),
    ("se-gz", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq.gz", "-S", "out.sam"], None),
    ("se-bz2", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq.bz2", "-S", "out.sam"], None),
    ("se-stdout", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq"], None),
    ("se-stdin", "hisat2", ["-x", "{TD}/idx", "-U", "-", "-S", "out.sam"], "se.fq"),
    ("se-commalist", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq,{TD}/se.fq.gz", "-S", "out.sam"], None),
    ("se-fasta", "hisat2", ["-x", "{TD}/idx", "-f", "-U", "{TD}/se.fa", "-S", "out.sam"], None),
    ("se-skip-rl", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--skip-read-lengths", "-S", "out.sam"], None),
    ("se-unsupported-ext", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.txt", "-S", "out.sam"], None),
    ("se-seqs-c", "hisat2", ["-x", "{TD}/idx", "-c", "-U", "{SEQ1},{SEQ2}", "-S", "out.sam"], None),
    ("pe-plain", "hisat2", ["-x", "{TD}/idx", "-1", "{TD}/m1.fq", "-2", "{TD}/m2.fq", "-S", "out.sam"], None),
    ("pe-attached", "hisat2", ["-x", "{TD}/idx", "-1{TD}/m1.fq", "-2{TD}/m2.fq", "-S", "out.sam"], None),
    ("se-un-al", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--un", "un.fq", "--al", "al.fq", "-S", "out.sam"], None),
    ("se-un-eq", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--un=un.fq", "-S", "out.sam"], None),
    ("se-un-gz-al-bz2", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--un-gz", "un.fq", "--al-bz2", "al.fq", "-S", "out.sam"], None),
    ("se-un-stdout", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--un", "un.fq"], None),
    ("se-un-dir", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--un", "{CWD}", "-S", "out.sam"], None),
    ("se-no-unal", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--no-unal", "-S", "out.sam"], None),
    ("se-no-unal-un", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--no-unal", "--un", "un.fq"], None),
    ("pe-conc-ext", "hisat2", ["-x", "{TD}/idx", "-1", "{TD}/m1.fq", "-2", "{TD}/m2.fq", "--un-conc", "uc.fq", "--al-conc", "ac.fq", "--al-conc-disc", "acd", "-S", "out.sam"], None),
    ("pe-conc-pct-gz", "hisat2", ["-x", "{TD}/idx", "-1", "{TD}/m1.fq", "-2", "{TD}/m2.fq", "--un-conc-gz", "uc_%.fq.gz", "--al-conc-bz2", "ac_%.fq.bz2", "-S", "out.sam"], None),
    ("pe-conc-dir", "hisat2", ["-x", "{TD}/idx", "-1", "{TD}/m1.fq", "-2", "{TD}/m2.fq", "--al-conc", "{CWD}", "-S", "out.sam"], None),
    ("large-auto", "hisat2", ["-x", "{TD}/lidx", "-U", "{TD}/se.fq", "-S", "out.sam"], None),
    ("large-forced", "hisat2", ["-x", "{TD}/lidx", "--large-index", "-U", "{TD}/se.fq", "-S", "out.sam"], None),
    ("large-forced-missing", "hisat2", ["-x", "{TD}/idx", "--large-index", "-U", "{TD}/se.fq", "-S", "out.sam"], None),
    ("missing-read", "hisat2", ["-x", "{TD}/idx", "-U", "nosuch.fq", "-S", "out.sam"], None),
    ("missing-index", "hisat2", ["-x", "nosuchidx", "-U", "{TD}/se.fq", "-S", "out.sam"], None),
    ("temp-directory", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--temp-directory", "{CWD}", "-S", "out.sam"], None),
    ("verbose-noreads", "hisat2", ["-x", "{TD}/idx", "--verbose"], None),
    ("verbose-reads", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/se.fq", "--verbose", "-S", "out.sam"], None),
    ("debug-missing", "hisat2", ["-x", "{TD}/idx", "--debug", "-U", "{TD}/se.fq", "-S", "out.sam"], None),
    # Paths with spaces and non-ASCII characters (setup copies the reads there).
    ("space-path", "hisat2", ["-x", "{TD}/idx", "-U", "{CWD}/a b/r 1.fq", "--un", "{CWD}/a b/un 1.fq", "-S", "{CWD}/a b/o 1.sam"], None),
    ("nonascii-path", "hisat2", ["-x", "{TD}/idx", "-U", "{CWD}/za\u017c\u00f3\u0142\u0107/r\u0119ads.fq", "--un-gz", "{CWD}/za\u017c\u00f3\u0142\u0107/\u0105.fq", "-S", "{CWD}/za\u017c\u00f3\u0142\u0107/o.sam"], None),
    # Read-length check stopping at a blank line (the script warns "list index out of range").
    ("blank-line-rl", "hisat2", ["-x", "{TD}/idx", "-U", "{TD}/blank.fq", "-S", "out.sam"], None),
    # `--`: wrapper options before it (only reads, --verbose and --log-file act; the rest
    # are dropped), binary options after it.
    ("dd-first", "hisat2", ["--", "-x", "{TD}/idx", "-U", "{TD}/se.fq", "-S", "out.sam"], None),
    ("dd-split", "hisat2", ["-U", "{TD}/se.fq", "--no-unal", "-p", "2", "--", "-x", "{TD}/idx", "--un", "un.fq", "-S", "out.sam"], None),
    ("dd-verbose-pe", "hisat2", ["--verbose", "-1", "{TD}/m1.fq", "--2={TD}/m2.fq", "--", "-x", "{TD}/idx", "-S", "out.sam"], None),
    ("dd-log-file", "hisat2", ["--log-file", "log.txt", "--reads", "{TD}/se.fq", "--", "-x", "{TD}/idx", "-S", "out.sam"], None),
    ("dd-commalist", "hisat2", ["-U", "{TD}/se.fq,{TD}/se.fq", "--", "-x", "{TD}/idx", "-S", "out.sam"], None),
    ("build-noargs", "hisat2-build", [], None),
    ("build", "hisat2-build", ["-q", "{TD}/ref.fa", "bidx"], None),
    ("build-verbose", "hisat2-build", ["-q", "--verbose", "{TD}/ref.fa", "bidx"], None),
    ("build-large", "hisat2-build", ["-q", "--large-index", "{TD}/ref.fa", "bidx"], None),
    ("inspect-noargs", "hisat2-inspect", [], None),
    ("inspect-summary", "hisat2-inspect", ["-s", "{TD}/idx"], None),
    ("inspect-names-large", "hisat2-inspect", ["-n", "{TD}/lidx"], None),
    ("inspect-verbose", "hisat2-inspect", ["--verbose", "-n", "{TD}/idx"], None),
]

SEQ1 = SEQ2 = None


def load_seqs():
    global SEQ1, SEQ2
    lines = open(os.path.join(TD, "se.fq")).read().split("\n")
    SEQ1, SEQ2 = lines[1], lines[5]


def norm(text, tooldir, cwd):
    """Replace the run's directories by tags. Each path may appear as given or
    resolved (the launcher resolves symlinks, e.g. macOS /tmp -> /private/tmp),
    with either separator; longer spellings go first."""
    forms = []
    for path, tag in ((cwd, "<CWD>"), (tooldir, "<BIN>"), (TD, "<TD>")):
        for p in {path, os.path.realpath(path)}:
            forms += [(f, tag) for f in {p, p.replace("\\", "/"), p.replace("/", "\\")}]
    for form, tag in sorted(forms, key=lambda ft: -len(ft[0])):
        text = text.replace(form, tag)
    return text


def content(path):
    raw = open(path, "rb").read()
    if raw[:2] == b"\x1f\x8b":
        return "gzip", gzip.decompress(raw)
    if raw[:3] == b"BZh":
        return "bzip2", bz2.decompress(raw)
    return "plain", raw


def run(tooldir, case, cwd):
    name, tool, args, stdin = case
    shutil.rmtree(cwd, ignore_errors=True)
    os.makedirs(cwd)
    for a in args:                          # reads under {CWD}/... are copies of se.fq
        if a.startswith("{CWD}/") and (a.endswith("r 1.fq") or a.endswith("ads.fq")):
            dst = a.replace("{CWD}", cwd)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy(os.path.join(TD, "se.fq"), dst)
    argv = [os.path.join(tooldir, tool)] + [
        a.replace("{TD}", TD).replace("{CWD}", cwd).replace("{SEQ1}", SEQ1).replace("{SEQ2}", SEQ2) for a in args]
    inp = open(os.path.join(TD, stdin), "rb") if stdin else subprocess.DEVNULL
    p = subprocess.run(argv, cwd=cwd, stdin=inp, capture_output=True)
    files = {}
    for root, _, fs in os.walk(cwd):
        for f in fs:
            full = os.path.join(root, f)
            kind, data = content(full)
            if os.path.basename(full) in ("r 1.fq", "r\u0119ads.fq"):
                continue
            files[os.path.relpath(full, cwd).replace(os.sep, "/")] = (kind, norm(data.decode("latin-1"), tooldir, cwd))
    return {
        "exit": p.returncode,
        "stdout": norm(p.stdout.decode("latin-1"), tooldir, cwd),
        "stderr": norm(p.stderr.decode("latin-1"), tooldir, cwd),
        "files": files,
    }


def diff(a, b, label):
    d = list(difflib.unified_diff(a.splitlines(), b.splitlines(), "A (reference)", "B (launcher)", lineterm="", n=1))
    return [f"    {label}:"] + ["      " + x for x in d[:16]] + (["      ..."] if len(d) > 16 else [])


def xnorm(text):
    """Cross-platform: separators and the .exe suffix."""
    return text.replace("\\", "/").replace(".exe", "")


def compare(r, n):
    problems = []
    if r["exit"] != n["exit"]:
        problems.append(f"    exit: {r['exit']} vs {n['exit']}")
    for k in ("stdout", "stderr"):
        if r[k] != n[k]:
            problems += diff(r[k], n[k], k)
    for f in sorted(set(r["files"]) | set(n["files"])):
        if f not in r["files"] or f not in n["files"]:
            problems.append(f"    file {f}: only in {'B' if f in n['files'] else 'A'}")
        elif r["files"][f] != n["files"][f]:
            (rk, rc), (nk, nc) = r["files"][f], n["files"][f]
            if rk != nk:
                problems.append(f"    file {f}: {rk} vs {nk}")
            if rc != nc:
                problems += diff(rc, nc, f"file {f}")
    return problems


# Known, deliberate differences (see README.md "The launchers"): case -> reason.
EXPECTED = {
    "se-gz": "@PG command line: Perl feeds gzip input through a temporary pipe",
    "se-commalist": "@PG command line: Perl feeds gzip input through a temporary pipe",
    "se-bz2": "@PG command line: Perl feeds bzip2 input through a temporary pipe",
    "verbose-reads": "Perl drops the (INFO) lines once a read file is given",
    "space-path": "Perl runs the binary through an unquoted shell command line",
    "debug-missing": "the 'cannot run' message comes from sh under Perl",
}
# Windows vs macOS launcher: differences in the binaries themselves.
EXPECTED_XPLAT = {
    "align-noargs": "usage: Windows build has no --mm (memory-mapping off)",
    "align-help": "usage: Windows build has no --mm (memory-mapping off)",
    "verbose-noreads": "usage: Windows build has no --mm (memory-mapping off)",
    "align-version": "--version prints build host, date and type sizes",
    "verbose-reads": "binary's verbose timestamps",
    "debug-missing": "Windows cannot start the missing binary at all (exit 1)",
    "dd-commalist": "OS error text: the joined path has a second drive colon, invalid on Windows (EINVAL), missing elsewhere (ENOENT)",
}


def report(name, problems, expected, extra=""):
    """Print one case; return 1 if it differs unexpectedly."""
    status = "SAME" if not problems else ("EXPECTED-DIFF" if name in expected else "DIFF")
    print(f"{status}  {name}{extra}" + (f"  ({expected[name]})" if status == "EXPECTED-DIFF" else ""))
    if status == "DIFF":
        for line in problems:
            print(line)
    return status == "DIFF"


def main():
    if MODE == "cmp":
        a, b = (json.load(open(p, encoding="utf-8")) for p in sys.argv[2:4])
        fix = lambda d: {"exit": d["exit"], "stdout": xnorm(d["stdout"]), "stderr": xnorm(d["stderr"]),
                         "files": {k: (v[0], xnorm(v[1])) for k, v in d["files"].items()}}
        failed = sum(report(name, compare(fix(a[name]), fix(b[name])), EXPECTED_XPLAT)
                     for name in a if name in b)
    else:
        load_seqs()
        shutil.copy(os.path.join(TD, "se.fq"), os.path.join(TD, "se.txt"))
        if MODE == "dump":
            out = {}
            for case in CASES:
                if ONLY in case[0]:
                    out[case[0]] = run(NEW, case, os.path.join(WORK, case[0]))
                    print("ran", case[0], "exit", out[case[0]]["exit"])
            json.dump(out, open(OUT, "w", encoding="utf-8"), indent=1)
            return
        failed = 0
        for case in CASES:
            if ONLY in case[0]:
                r = run(REF, case, os.path.join(WORK, "ref", case[0]))
                n = run(NEW, case, os.path.join(WORK, "new", case[0]))
                failed += report(case[0], compare(r, n), EXPECTED)
    print(f"\n{failed} unexpected difference(s)")
    sys.exit(1 if failed else 0)


main()
