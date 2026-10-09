/*
 * hisat2_launcher.c - native versions of the `hisat2`, `hisat2-build` and
 * `hisat2-inspect` wrapper scripts (upstream: Perl and Python).
 *
 * On Windows a script is not a runnable command (cmd, Explorer and other
 * programs only start .exe files), so the Makefile builds these launchers there
 * instead. One source, compiled once per tool:
 *   -DLAUNCH_ALIGN    hisat2          port of the Perl `hisat2`  (links zlib, libbz2)
 *   -DLAUNCH_BUILD    hisat2-build    port of the Python `hisat2-build`
 *   -DLAUNCH_INSPECT  hisat2-inspect  port of the Python `hisat2-inspect`
 * As with the scripts, the hisat2-*-s/-l binaries must sit next to the
 * launcher. Each launcher runs the command the script would run and prints the
 * script's messages; the few deliberate differences are listed in README.md.
 * The code is portable so it can be compared with the scripts on Linux/macOS.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define EXE ".exe"
#define SEP '\\'
typedef struct _stat64 stat_t;
#define stat_fn _stat64
#else
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define EXE ""
#define SEP '/'
typedef struct stat stat_t;
#define stat_fn stat
#endif

#if !defined(LAUNCH_ALIGN) && !defined(LAUNCH_BUILD) && !defined(LAUNCH_INSPECT)
#error "define LAUNCH_ALIGN, LAUNCH_BUILD or LAUNCH_INSPECT"
#endif

/* ---- small helpers ------------------------------------------------------ */

static void oom(void) { fputs("hisat2: out of memory\n", stderr); exit(1); }
static void *xrealloc(void *p, size_t n) { if (!(p = realloc(p, n ? n : 1))) oom(); return p; }
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; return memcpy(xrealloc(NULL, n), s, n); }

static char *fmt(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int n = vsnprintf(NULL, 0, f, ap);
	va_end(ap);
	char *s = xrealloc(NULL, (size_t)n + 1);
	va_start(ap, f);
	vsnprintf(s, (size_t)n + 1, f, ap);
	va_end(ap);
	return s;
}

/* A growable, NULL-terminated string list (argument vectors). */
typedef struct { char **v; int n, cap; } list;

static void push(list *l, const char *s)
{
	if (l->n + 2 > l->cap) {
		l->cap = l->cap ? 2 * l->cap : 16;
		l->v = xrealloc(l->v, (size_t)l->cap * sizeof *l->v);
	}
	l->v[l->n++] = xstrdup(s);
	l->v[l->n] = NULL;
}

static char *join(char *const *v, int n, const char *sep)
{
	size_t len = 1;
	for (int i = 0; i < n; i++) len += strlen(v[i]) + strlen(sep);
	char *s = xrealloc(NULL, len);
	*s = 0;
	for (int i = 0; i < n; i++) {
		if (i) strcat(s, sep);
		strcat(s, v[i]);
	}
	return s;
}

#ifndef LAUNCH_BUILD
static int exists(const char *p) { stat_t st; return stat_fn(p, &st) == 0; }
#endif

static char *last_sep(const char *p)
{
	char *s = strrchr(p, '/');
#ifdef _WIN32
	char *b = strrchr(p, '\\');
	if (!s || (b && b > s)) s = b;
#endif
	return s;
}

/* Directory holding this launcher, with a trailing separator (Perl's $script_path). */
static char *self_dir(const char *argv0)
{
	char *path = NULL;
#ifdef _WIN32
	static char buf[32768];
	DWORD n = GetModuleFileNameA(NULL, buf, sizeof buf);
	(void)argv0;
	if (n && n < sizeof buf) path = xstrdup(buf);
#else
	char buf[PATH_MAX];
	if (strchr(argv0, '/')) {
		if (realpath(argv0, buf)) path = xstrdup(buf);
	} else {                                 /* found through PATH */
		char *dirs = xstrdup(getenv("PATH") ? getenv("PATH") : "");
		for (char *d = strtok(dirs, ":"); d && !path; d = strtok(NULL, ":")) {
			char *cand = fmt("%s/%s", d, argv0);
			if (access(cand, X_OK) == 0 && realpath(cand, buf)) path = xstrdup(buf);
			free(cand);
		}
		free(dirs);
	}
#endif
	if (!path || !last_sep(path)) {
		fputs("hisat2: cannot locate the launcher's own directory\n", stderr);
		exit(1);
	}
	last_sep(path)[1] = 0;
	return path;
}

/* LF-only stdout/stderr on Windows, like the patched binaries' _setmode(). */
static void binary_stdio(void)
{
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
#endif
}

/* ---- running a binary ----------------------------------------------------
 * start() runs `prog` with `argv` (argv[0] is the name the child sees). With
 * `out`, the child's stdout comes back as a binary stream; otherwise the child
 * shares our stdin/stdout/stderr. finish() returns the exit code, or -1 with
 * *sig set if the child was killed by a signal (POSIX only). */

#ifdef _WIN32
typedef intptr_t child;

/* Quote one argument so the child's C runtime parses it back unchanged. */
static char *win_quote(const char *a)
{
	if (*a && !strpbrk(a, " \t\n\v\"")) return xstrdup(a);
	char *o = xrealloc(NULL, 2 * strlen(a) + 3), *p = o;
	*p++ = '"';
	for (const char *s = a;; s++) {
		size_t bs = 0;
		while (*s == '\\') { bs++; s++; }
		if (!*s) { memset(p, '\\', 2 * bs); p += 2 * bs; break; }
		if (*s == '"') { memset(p, '\\', 2 * bs + 1); p += 2 * bs + 1; }
		else { memset(p, '\\', bs); p += bs; }
		*p++ = *s;
	}
	*p++ = '"';
	*p = 0;
	return o;
}

static int start(const char *prog, char **argv, FILE **out, child *c)
{
	list q = {0};
	for (int i = 0; argv[i]; i++) {
		char *s = win_quote(argv[i]);
		push(&q, s);
		free(s);
	}
	int fds[2], saved = -1;
	fflush(stdout);
	if (out) {                               /* MSVC docs' _pipe + _dup2 + _spawnv pattern */
		if (_pipe(fds, 1 << 16, _O_BINARY | _O_NOINHERIT)) return -1;
		saved = _dup(1);
		_dup2(fds[1], 1);
		_close(fds[1]);
	}
	*c = _spawnv(_P_NOWAIT, prog, (const char *const *)q.v);
	int err = errno;
	if (out) {
		_dup2(saved, 1);
		_close(saved);
		if (*c == -1) _close(fds[0]);
		else *out = _fdopen(fds[0], "rb");
	}
	errno = err;
	return *c == -1 ? -1 : 0;
}

static int finish(child *c, int *sig)
{
	int st;
	*sig = 0;
	return _cwait(&st, *c, 0) == -1 ? 1 : st;
}
#else
typedef pid_t child;

static int start(const char *prog, char **argv, FILE **out, child *c)
{
	int fds[2];
	if (out && pipe(fds)) return -1;
	fflush(stdout);
	fflush(stderr);
	if ((*c = fork()) < 0) return -1;
	if (*c == 0) {
		if (out) { dup2(fds[1], 1); close(fds[0]); close(fds[1]); }
		execv(prog, argv);
		fprintf(stderr, "hisat2: cannot run '%s': %s\n", prog, strerror(errno));
		_exit(127);
	}
	if (out) { close(fds[1]); *out = fdopen(fds[0], "r"); }
	return 0;
}

static int finish(child *c, int *sig)
{
	int st;
	*sig = 0;
	while (waitpid(*c, &st, 0) < 0)
		if (errno != EINTR) return 1;
	if (WIFSIGNALED(st)) { *sig = WTERMSIG(st); return -1; }
	return WEXITSTATUS(st);
}
#endif

/* ======================================================================== */
#ifdef LAUNCH_ALIGN
/* Port of the Perl `hisat2` wrapper. */

#include <bzlib.h>
#include <zlib.h>

#define MAX_READS 10000        /* hisat2_read_statistics.py default (-n) */
#define MAX_SKIP_LINES 10000

static int verbose;

static int ieq(const char *a, const char *b)   /* ASCII case-insensitive == */
{
	for (; *a && *b; a++, b++)
		if ((*a | 32) != (*b | 32)) return 0;
	return *a == *b;
}

static int is_file(const char *p) { stat_t st; return stat_fn(p, &st) == 0 && S_ISREG(st.st_mode); }
static int is_dir(const char *p) { stat_t st; return stat_fn(p, &st) == 0 && S_ISDIR(st.st_mode); }

/* Perl's Info(): "(INFO): ..." on stderr when --verbose is given. */
static void info(const char *f, ...)
{
	if (!verbose) return;
	va_list ap;
	fputs("(INFO): ", stderr);
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);
}

/* Perl's Fail(): "(ERR): ...", then die's message and exit status. */
static void fail(int code, const char *f, ...)
{
	va_list ap;
	fputs("(ERR): ", stderr);
	va_start(ap, f);
	vfprintf(stderr, f, ap);
	va_end(ap);
	fputs("Exiting now ...\n", stderr);
	exit(code);
}

/* ---- read-length check (port of hisat2_read_statistics.py) ---- */

typedef struct {
	gzFile gz;                               /* plain and gzip input */
	FILE *f;                                 /* bzip2 input */
	BZFILE *bz;
	char buf[1 << 16];
	int len, pos;
} rsrc;

/* Next bzip2 bytes, continuing through concatenated streams (as Python's bz2). */
static int bz_read(rsrc *r)
{
	while (r->bz) {
		int err, n = BZ2_bzRead(&err, r->bz, r->buf, sizeof r->buf);
		if (err == BZ_OK) return n;
		if (err != BZ_STREAM_END) return 0;
		void *u;
		int nu;
		char next[BZ_MAX_UNUSED];
		BZ2_bzReadGetUnused(&err, r->bz, &u, &nu);
		memcpy(next, u, (size_t)nu);
		BZ2_bzReadClose(&err, r->bz);
		r->bz = (nu > 0 || !feof(r->f)) ? BZ2_bzReadOpen(&err, r->f, 0, 0, next, nu) : NULL;
		if (r->bz && err != BZ_OK) { BZ2_bzReadClose(&err, r->bz); r->bz = NULL; }
		if (n > 0) return n;
	}
	return 0;
}

/* Next line without its newline ("" for a blank line), or NULL at the end. */
static char *rs_line(rsrc *r, char **line, size_t *cap)
{
	size_t n = 0;
	int got = 0;
	for (;;) {
		if (r->pos >= r->len) {
			int k = r->gz ? gzread(r->gz, r->buf, sizeof r->buf) : bz_read(r);
			if (k <= 0) break;
			r->len = k;
			r->pos = 0;
		}
		got = 1;
		char *s = r->buf + r->pos, *nl = memchr(s, '\n', (size_t)(r->len - r->pos));
		size_t k = nl ? (size_t)(nl - s) : (size_t)(r->len - r->pos);
		if (n + k + 1 > *cap) *line = xrealloc(*line, *cap = 2 * (n + k + 1));
		memcpy(*line + n, s, k);
		n += k;
		r->pos += (int)k + (nl != NULL);
		if (nl) break;
	}
	if (!got) return NULL;
	(*line)[n] = 0;
	return *line;
}

static size_t stripped_len(const char *s)  /* len(s.strip()) */
{
	const char *e = s + strlen(s);
	while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
	while (e > s && (e[-1] == ' ' || (e[-1] >= '\t' && e[-1] <= '\r'))) e--;
	return (size_t)(e - s);
}

static int has_token(const char *s)        /* line[1:].split()[0] exists */
{
	return stripped_len(s) > 0;
}

typedef struct { size_t len, count; } lencount;

static int by_count_then_len(const void *a, const void *b)
{
	const lencount *x = a, *y = b;
	if (x->count != y->count) return x->count < y->count ? 1 : -1;
	return x->len < y->len ? 1 : (x->len > y->len ? -1 : 0);
}

/* The read lengths of the first MAX_READS reads, most frequent first (ties:
 * longer first), comma-separated -- or NULL. Problems are reported as the
 * script's "Warning: ..." lines and leave whatever was counted so far. */
static char *read_lengths(const char *fn)
{
	enum { NONE, GZ, BZ2 } comp = NONE;
	char *name = xstrdup(fn), *dot = strrchr(name, '.'), *ext = dot ? dot + 1 : name;
	if (ieq(ext, "gz") || ieq(ext, "bz2")) {
		comp = ieq(ext, "gz") ? GZ : BZ2;
		if (dot) *dot = 0;
		dot = strrchr(name, '.');
		ext = dot ? dot + 1 : name;
	}
	char marker = 0;
	const char *fa[] = {"fa", "fasta", "fna"}, *fq[] = {"fq", "fastq"};
	for (int i = 0; i < 3; i++) if (ieq(ext, fa[i])) marker = '>';
	for (int i = 0; i < 2; i++) if (ieq(ext, fq[i])) marker = '@';
	free(name);

	rsrc *r = xrealloc(NULL, sizeof *r);
	memset(r, 0, sizeof *r);
	r->f = fopen(fn, "rb");
	if (!r->f) {
		fprintf(stderr, "Warning: [Errno %d] %s: '%s'\n", errno, strerror(errno), fn);
		free(r);
		return NULL;
	}
	if (comp == BZ2) {
		int err;
		r->bz = BZ2_bzReadOpen(&err, r->f, 0, 0, NULL, 0);
	} else {
		fclose(r->f);
		r->f = NULL;
		r->gz = gzopen(fn, "rb");
	}

	size_t *counts = NULL, ncounts = 0, cap = 0, total = 0;
	char *line = NULL;
	if (!marker) {
		fputs("Warning: Unsupported file format\n", stderr);
		goto done;
	}
	int skip = 0;
	char *l = NULL;
	for (; skip < MAX_SKIP_LINES; skip++) {
		if (!(l = rs_line(r, &line, &cap))) goto done;
		if (l[0] == marker) break;
	}
	if (skip == MAX_SKIP_LINES) {
		fputs("Warning: Invalid file format\n", stderr);
		goto done;
	}
	for (;;) {
		if (!has_token(l + 1)) {
			fputs("Warning: list index out of range\n", stderr);
			break;
		}
		size_t len = 0;
		if (marker == '@') {             /* FASTQ: id, sequence, '+', quality */
			if (!(l = rs_line(r, &line, &cap))) break;
			len = stripped_len(l);
		} else {                         /* FASTA: sequence lines up to the next '>' */
			while ((l = rs_line(r, &line, &cap)) && l[0] != '>') len += stripped_len(l);
		}
		if (len >= ncounts) {
			size_t m = 2 * len + 1;
			counts = xrealloc(counts, m * sizeof *counts);
			memset(counts + ncounts, 0, (m - ncounts) * sizeof *counts);
			ncounts = m;
		}
		counts[len]++;
		if (++total >= MAX_READS) break;
		if (marker == '@') {
			rs_line(r, &line, &cap);
			rs_line(r, &line, &cap);
			l = rs_line(r, &line, &cap);
		}
		if (!l) break;
	}
done:
	free(line);
	if (r->gz) gzclose(r->gz);
	if (r->bz) { int err; BZ2_bzReadClose(&err, r->bz); }
	if (r->f) fclose(r->f);
	free(r);
	if (!total) { free(counts); return NULL; }
	lencount *lc = xrealloc(NULL, ncounts * sizeof *lc);
	size_t k = 0;
	for (size_t i = 0; i < ncounts; i++)
		if (counts[i]) lc[k++] = (lencount){i, counts[i]};
	qsort(lc, k, sizeof *lc, by_count_then_len);
	char *s = xstrdup("");
	for (size_t i = 0; i < k; i++) {
		char *t = fmt("%s%s%zu", s, i ? "," : "", lc[i].len);
		free(s);
		s = t;
	}
	free(lc);
	free(counts);
	return strcmp(s, "0") ? s : NULL;
}

/* ---- --un/--al/--un-conc/--al-conc[-disc] outputs ---- */

typedef struct { FILE *f; gzFile gz; BZFILE *bz; } sink;

static int sink_open(sink *s, const char *path, int comp)
{
	memset(s, 0, sizeof *s);
	if (comp == 1) return (s->gz = gzopen(path, "wb")) != NULL;
	if (!(s->f = fopen(path, "wb"))) return 0;
	if (comp == 2) {
		int err;
		s->bz = BZ2_bzWriteOpen(&err, s->f, 9, 0, 0);
		if (err != BZ_OK) return 0;
	}
	return 1;
}

static void sink_write(sink *s, const char *p, size_t n)
{
	int err;
	if (s->gz) gzwrite(s->gz, p, (unsigned)n);
	else if (s->bz) BZ2_bzWrite(&err, s->bz, (void *)p, (int)n);
	else fwrite(p, 1, n, s->f);
}

static void sink_close(sink *s)
{
	int err;
	if (s->gz) gzclose(s->gz);
	if (s->bz) BZ2_bzWriteClose(&err, s->bz, 0, NULL, NULL);
	if (s->f) fclose(s->f);
}

/* Wrapper read-redirection options, in the Perl loop's order. */
enum { UN_CONC, AL_CONC, AL_CONC_DISC, UN, AL, NREAD };
static const char *read_opt[NREAD] = {"un-conc", "al-conc", "al-conc-disc", "un", "al"};

static int get_line(FILE *f, char **buf, size_t *cap)  /* chomped; 0 at end */
{
	size_t n = 0;
	if (!*buf) *buf = xrealloc(NULL, *cap = 1 << 16);
	for (;;) {
		if (!fgets(*buf + n, (int)(*cap - n), f)) return n > 0;
		n += strlen(*buf + n);
		if (n && (*buf)[n - 1] == '\n') { (*buf)[n - 1] = 0; return 1; }
		*buf = xrealloc(*buf, *cap *= 2);
	}
}

static int hexval(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* --passthrough read line: the read with whitespace escaped as %XX. */
static size_t unescape(const char *s, char *o)
{
	char *p = o;
	while (*s) {
		int hi = hexval(s[1]), lo = hi < 0 ? -1 : hexval(s[2]);
		if (s[0] == '%' && lo >= 0) {
			*p++ = (char)(hi * 16 + lo);
			s += 3;
		} else *p++ = *s++;
	}
	return (size_t)(p - o);
}

/* Split a target into directory and file name (Perl's splitpath). */
static char *in_dir(const char *dir, const char *file)
{
	size_t n = strlen(dir);
	if (!n) return xstrdup(file);
	int sep = dir[n - 1] == '/' || dir[n - 1] == SEP;
	return fmt("%s%s%s", dir, sep ? "" : (char[]){SEP, 0}, file);
}

int main(int argc, char **argv)
{
	binary_stdio();
	char *dir = self_dir(argv[0]);
	char *prog_s = fmt("%shisat2-align-s" EXE, dir), *prog_l = fmt("%shisat2-align-l" EXE, dir);
	if (!is_file(prog_s))
		fail(2, "Expected hisat2 to be in same directory with hisat2-align-s and hisat2-align-l:\n%s\n", dir);

	/* Arguments, with surrounding whitespace removed (as Perl does). */
	list args = {0};
	for (int i = 1; i < argc; i++) {
		const char *s = argv[i], *e = s + strlen(s);
		while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
		while (e > s && (e[-1] == ' ' || (e[-1] >= '\t' && e[-1] <= '\r'))) e--;
		char *t = fmt("%.*s", (int)(e - s), s);
		push(&args, t);
		free(t);
	}

	/* Capture the arguments the wrapper handles; the rest go to the binary. */
	list bin = {0}, wrap = {0}, unps = {0}, m1 = {0}, m2 = {0};
	int debug = 0, no_unal = 0, large = 0, skip_stat = 0, seq_in_args = 0;
	char *read_fn[NREAD] = {0};
	int read_comp[NREAD] = {0};
	for (int i = 0; i < args.n; i++) {
		char *a = args.v[i], *eq = strchr(a, '=');
		char *name = eq ? fmt("%.*s", (int)(eq - a), a) : xstrdup(a);
		const char *val = eq && eq[1] ? eq + 1 : NULL;
		const char *mate = NULL;            /* -1 / --1 / -2 / --2, not --12 */
		if (name[0] == '-') {
			const char *t = name + 1 + (name[1] == '-');
			if ((*t == '1' || *t == '2') && !(t[0] == '1' && t[1] == '2')) mate = t;
		}
		list *reads = !strcmp(name, "-U") || !strcmp(name, "--unpaired") ? &unps
		            : mate ? (*mate == '1' ? &m1 : &m2) : NULL;
		if (reads) {
			const char *v = mate && mate[1] ? mate + 1 : val;
			if (!v) {
				if (i + 1 >= args.n) fail(255, "Argument expected in next token!\n");
				v = args.v[++i];
			}
			char *copy = xstrdup(v);
			const char *opt = reads == &unps ? "-U" : (reads == &m1 ? "-1" : "-2");
			for (char *f = strtok(copy, ","); f; f = strtok(NULL, ",")) {
				push(reads, f);
				push(&wrap, opt);
				push(&wrap, f);
			}
			free(copy);
		} else if (!strcmp(name, "--debug")) debug = 1;
		else if (!strcmp(name, "--no-unal")) no_unal = 1;
		else if (!strcmp(name, "--large-index")) large = 1;
		else if (!strcmp(name, "--skip-read-lengths")) skip_stat = 1;
		else if (!strcmp(a, "--temp-directory")) {   /* only for the script's own temp files */
			push(&wrap, a);
			if (i + 1 < args.n) push(&wrap, args.v[++i]);
		} else {
			int k = 0;
			for (; k < NREAD; k++) {
				char *plain = fmt("--%s", read_opt[k]), *gz = fmt("--%s-gz", read_opt[k]), *bz = fmt("--%s-bz2", read_opt[k]);
				int comp = !strcmp(name, plain) ? 0 : !strcmp(name, gz) ? 1 : !strcmp(name, bz) ? 2 : -1;
				free(plain); free(gz); free(bz);
				if (comp < 0) continue;
				if (!val) {
					if (i + 1 >= args.n) fail(255, "--%s* option takes an argument.\n", read_opt[k]);
					val = args.v[++i];
				}
				read_fn[k] = xstrdup(val);
				read_comp[k] = comp;
				break;
			}
			if (k == NREAD) {
				if (!strcmp(a, "-c")) seq_in_args = 1;
				if (!strcmp(a, "--verbose")) verbose = 1;  /* also forwarded, as in Perl */
				push(&bin, a);
			}
		}
		free(name);
	}

	/* Read redirection or --no-unal: the binary emits each read after its SAM
	 * record (--passthrough) and the wrapper writes SAM to -S itself. */
	int passthru = no_unal;
	for (int k = 0; k < NREAD; k++) passthru |= read_fn[k] != NULL;
	const char *cap_out = NULL;
	if (passthru) {
		list kept = {0};
		push(&bin, "--passthrough");
		cap_out = "-";
		for (int i = 0; i < bin.n; i++) {
			const char *a = bin.v[i];
			if (!strcmp(a, "-S") || !strcmp(a, "--output")) {
				if (i + 1 >= bin.n) fail(255, "-S/--output takes an argument.\n");
				cap_out = bin.v[++i];
			} else if (!strncmp(a, "-S", 2) && a[2]) cap_out = a + 2;
			else if (!strncmp(a, "--output=", 9)) cap_out = a + 9;
			else push(&kept, a);
		}
		bin = kept;
	}

	char *w = join(wrap.v, wrap.n, " "), *b = join(bin.v, bin.n, " ");
	info("Before arg handling:\n");
	info("  Wrapper args:\n[ %s ]\n", w);
	info("  Binary args:\n[ %s ]\n", b);

	/* Read lengths of the first read file, passed as --read-lengths. */
	list *first = unps.n ? &unps : &m1;
	if (first->n && !seq_in_args && !skip_stat && strcmp(first->v[0], "-")) {
		info("Check read length: %s\n", first->v[0]);
		char *rl = read_lengths(first->v[0]);
		if (rl) {
			info("Read Length String: %s\n", rl);
			push(&bin, "--read-lengths");
			push(&bin, rl);
		}
	}
	if (!seq_in_args) {
		list *all[] = {&unps, &m1, &m2};
		for (int k = 0; k < 3; k++)
			for (int i = 0; i < all[k]->n; i++)
				if (!is_file(all[k]->v[i]) && strcmp(all[k]->v[i], "-"))
					fail(2, "Read file '%s' doesn't exist\n", all[k]->v[i]);
	}
	list *reads[] = {&m1, &m2, &unps};
	const char *ropt[] = {"-1", "-2", "-U"};
	for (int k = 0; k < 3; k++)
		if (reads[k]->n) {
			char *j = join(reads[k]->v, reads[k]->n, ",");
			push(&bin, ropt[k]);
			push(&bin, j);
			free(j);
		}

	b = join(bin.v, bin.n, " ");
	info("After arg handling:\n");
	info("  Binary args:\n[ %s ]\n", b);

	/* Index name (-x), for choosing the small or large binary. */
	char *idx = NULL;
	for (int i = 0; i < bin.n && !idx; i++) {
		const char *a = bin.v[i];
		if ((!strcmp(a, "-x") || !strcmp(a, "--index")) && i + 1 < bin.n) idx = xstrdup(bin.v[i + 1]);
		else if (!strncmp(a, "-x", 2) && a[2] && a[2] != '-') idx = xstrdup(a + 2);
		else if (!strncmp(a, "--index=", 8)) idx = xstrdup(a + 8);
	}
	if (idx) {
		char *s = fmt("%s.1.ht2", idx), *l = fmt("%s.1.ht2l", idx);
		if (!exists(s) && !exists(l)) {
			const char *env = getenv("HISAT2_INDEXES");
			char *alt = env ? in_dir(env, idx) : NULL;
			char *as = alt ? fmt("%s.1.ht2", alt) : NULL, *al = alt ? fmt("%s.1.ht2l", alt) : NULL;
			if (!alt || (!exists(as) && !exists(al))) fail(255, "\"%s\" does not exist\n", idx);
			idx = alt;
		}
	} else {
		info("Cannot find any index option (--reference-string, --ref-string or -x) in the given command line.\n");
		idx = "";
	}

	const char *prog = prog_s;
	char *large_idx = fmt("%s.1.ht2l", idx), *small_idx = fmt("%s.1.ht2", idx);
	if (large) {
		info("Using a large index enforced by user.\n");
		prog = prog_l;
		if (!is_file(large_idx)) fail(2, "Cannot find the large index %s\n", large_idx);
		info("Using large index (%s).\n", large_idx);
	} else if (is_file(large_idx) && !is_file(small_idx)) {
		info("Cannot find a small index but a large one seems to be present.\n");
		info("Switching to using the large index (%s).\n", large_idx);
		prog = prog_l;
	} else {
		info("Using the small index (%s).\n", small_idx);
	}
	if (debug) prog = fmt("%.*s-debug" EXE, (int)(strlen(prog) - strlen(EXE)), prog);

	info("'%s' --wrapper basic-0 %s\n", prog, b);
	list cmd = {0};
	push(&cmd, prog);
	push(&cmd, "--wrapper");
	push(&cmd, "basic-0");
	for (int i = 0; i < bin.n; i++) push(&cmd, bin.v[i]);

	child c;
	FILE *ht = NULL;
	if (start(prog, cmd.v, cap_out ? &ht : NULL, &c)) {
		fprintf(stderr, "(ERR): Failed to execute hisat2-align: %s\n", strerror(errno));
		return 1;
	}
	if (cap_out) {
		FILE *sam = stdout;
		if (strcmp(cap_out, "-") && !(sam = fopen(cap_out, "wb")))
			fail(errno ? errno : 255, "Could not open output file '%s' for writing.\n", cap_out);
		/* Output files, opened in the Perl order; conc options make a mate-1/2 pair. */
		static const int order[NREAD] = {AL, UN, AL_CONC, AL_CONC_DISC, UN_CONC};
		sink out[NREAD][2];
		int open_[NREAD] = {0}, nsink = 0;
		for (int o = 0; o < NREAD; o++) {
			int k = order[o];
			if (!read_fn[k]) continue;
			const char *t = read_fn[k], *sep = last_sep(t);
			char *pdir = sep ? fmt("%.*s", (int)(sep - t + 1), t) : xstrdup("");
			const char *base = sep ? sep + 1 : t;
			if (is_dir(t)) { pdir = xstrdup(t); base = ""; }
			if (k == UN || k == AL) {
				char *fn = *base ? xstrdup(t) : in_dir(pdir, fmt("%s-seqs", read_opt[k]));
				if (!sink_open(&out[k][0], fn, read_comp[k]))
					fail(errno ? errno : 255, "Could not open --%s output file '%s'\n", read_opt[k], t);
			} else {
				char *fn = *base ? xstrdup(base) : fmt("%s-mate", read_opt[k]);
				char *fns[2];
				for (int m = 0; m < 2; m++) {
					char *pct = strchr(fn, '%'), *d = strrchr(fn, '.'), *mf;
					if (pct) {                 /* every '%' becomes the mate number */
						mf = xstrdup(fn);
						for (char *p = mf; *p; p++) if (*p == '%') *p = (char)('1' + m);
					} else if (d) mf = fmt("%.*s.%d%s", (int)(d - fn), fn, m + 1, d);
					else mf = fmt("%s.%d", fn, m + 1);
					fns[m] = in_dir(pdir, mf);
				}
				for (int m = 0; m < 2; m++)
					if (!sink_open(&out[k][m], fns[m], read_comp[k]))
						fail(errno ? errno : 255, "Could not open --%s mate-%d output file '%s'\n", read_opt[k], m + 1, fns[m]);
			}
			open_[k] = 1;
			nsink++;
		}

		char *line = NULL, *rd = NULL, *buf = NULL;
		size_t cap = 0, rcap = 0;
		while (get_line(ht, &line, &cap)) {
			int filt = 0;
			if (line[0] != '@') {
				const char *tab = strchr(line, '\t');
				long fl = tab ? strtol(tab + 1, NULL, 10) : 0;
				int unal = (fl & 4) != 0, secondary = (fl & 256) != 0;
				filt = no_unal && unal;
				if (!get_line(ht, &rd, &rcap)) rd[0] = 0;
				if (nsink) {
					buf = xrealloc(buf, strlen(rd) + 1);
					size_t n = unescape(rd, buf);
					int mate1 = (fl & 64) != 0, mate2 = (fl & 128) != 0;
					if (!mate1 && !mate2 && !secondary) {
						int k = unal ? UN : AL;
						if (open_[k]) sink_write(&out[k][0], buf, n);
					}
					if ((mate1 || mate2) && !secondary) {
						int conc = (fl & 2) != 0, conc_disc = !(fl & 4) || !(fl & 8), m = mate1 ? 0 : 1;
						int k = conc ? AL_CONC : UN_CONC;
						if (open_[k]) sink_write(&out[k][m], buf, n);
						if (conc_disc && open_[AL_CONC_DISC]) sink_write(&out[AL_CONC_DISC][m], buf, n);
					}
				}
			}
			if (!filt) { fputs(line, sam); fputc('\n', sam); }
		}
		for (int k = 0; k < NREAD; k++)
			if (open_[k]) {
				sink_close(&out[k][0]);
				if (k != UN && k != AL) sink_close(&out[k][1]);
			}
		if (sam != stdout) fclose(sam); else fflush(stdout);
		fclose(ht);
	}

	int sig, ret = finish(&c, &sig);
	if (ret < 0) {
		static const struct { int n; const char *s; } names[] = {
#ifndef _WIN32
			{SIGHUP, "HUP"}, {SIGINT, "INT"}, {SIGQUIT, "QUIT"}, {SIGILL, "ILL"}, {SIGABRT, "ABRT"},
			{SIGFPE, "FPE"}, {SIGKILL, "KILL"}, {SIGBUS, "BUS"}, {SIGSEGV, "SEGV"}, {SIGPIPE, "PIPE"},
			{SIGTERM, "TERM"},
#endif
			{0, "(unknown)"}};
		const char *nm = "(unknown)";
		for (size_t i = 0; names[i].n; i++) if (names[i].n == sig) nm = names[i].s;
		fprintf(stderr, "(ERR): hisat2-align died with signal %d (%s) \n", sig, nm);
		return 1;
	}
	if (ret) fprintf(stderr, "(ERR): hisat2-align exited with value %d\n", ret);
	return ret;
}

/* ======================================================================== */
#else
/* Ports of the Python `hisat2-build` and `hisat2-inspect` wrappers. */

#ifdef LAUNCH_BUILD
#define TOOL "hisat2-build"
#else
#define TOOL "hisat2-inspect"
#endif

int main(int argc, char **argv)
{
	list a = {0};
	int large = 0, debug = 0, verbose = 0;
	binary_stdio();
	push(&a, argv[0]);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--large-index")) large = 1;
		else if (!strcmp(argv[i], "--debug")) debug = 1;
		else if (!strcmp(argv[i], "--verbose")) verbose = 1;
		else push(&a, argv[i]);
	}
	char *dir = self_dir(argv[0]);
	const char *dbg = debug ? "-debug" : "";
	char *spec = fmt("%s" TOOL "-s%s" EXE, dir, dbg), *spec_l = fmt("%s" TOOL "-l%s" EXE, dir, dbg);
	if (large) spec = spec_l;
#ifdef LAUNCH_BUILD
	/* Large index when the reference FASTA files exceed ~4 GiB. */
	else if (a.n >= 2) {
		long long total = 0;
		char *refs = xstrdup(a.v[a.n - 2]);
		for (char *f = strtok(refs, ","); f; f = strtok(NULL, ",")) {
			stat_t st;
			if (stat_fn(f, &st) == 0) total += (long long)st.st_size;
		}
		if (total > 4LL * 1024 * 1024 * 1024 - 200) spec = spec_l;
	}
#else
	/* Large binary when only the large index exists (also under $HISAT2_INDEXES). */
	else if (a.n >= 1) {
		const char *base = a.v[a.n - 1];
		char *l = fmt("%s.1.ht2l", base), *s = fmt("%s.1.ht2", base);
		int have_l = exists(l), have_s = exists(s);
		if (!have_l && !have_s) {
			const char *env = getenv("HISAT2_INDEXES");
			l = fmt("%s/%s.1.ht2l", env ? env : "", base);
			s = fmt("%s/%s.1.ht2", env ? env : "", base);
			have_l = exists(l);
			have_s = exists(s);
		}
		if (have_l && !have_s) spec = spec_l;
	}
#endif
	list cmd = {0};
	push(&cmd, TOOL);
	push(&cmd, "--wrapper");
	push(&cmd, "basic-0");
	for (int i = 1; i < a.n; i++) push(&cmd, a.v[i]);
	if (verbose) fprintf(stderr, "INFO: Command: %s %s\n", spec, join(cmd.v + 1, cmd.n - 1, " "));

	child c;
	if (start(spec, cmd.v, NULL, &c)) {
		fprintf(stderr, TOOL ": cannot run '%s': %s\n", spec, strerror(errno));
		return 1;
	}
	int sig, ret = finish(&c, &sig);
#ifndef _WIN32
	if (ret < 0) {                           /* died from a signal: pass it on, as exec would */
		signal(sig, SIG_DFL);
		raise(sig);
		return 128 + sig;
	}
#endif
	return ret;
}
#endif
