/*
 * gzip_reader.cpp
 *
 * Implementation of the thin zlib/libbz2 wrapper declared in gzip_reader.h.
 * This is the ONLY translation unit that includes <zlib.h> and <bzlib.h>;
 * everything else deals in
 * opaque handles. See gzip_reader.h for the rationale.
 */

#include "gzip_reader.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>
#include <zlib.h>

/* dup()/open()/read()/lseek(): POSIX <unistd.h>, or the _-prefixed versions
 * from <io.h> on Windows (where files must be opened in binary mode). */
#ifdef _WIN32
#include <io.h>
#define gzr_dup _dup
#define gzr_closefd _close
#define gzr_openfd(p) _open((p), _O_RDONLY | _O_BINARY)
#define gzr_readfd _read
#define gzr_lseek _lseek
#else
#include <unistd.h>
#define gzr_dup dup
#define gzr_closefd close
#define gzr_openfd(p) open((p), O_RDONLY)
#define gzr_readfd read
#define gzr_lseek lseek
#endif

/* A handle reads through zlib (gzip and plain input) or, for a bzip2 file,
 * through libbz2 (f + bz). */
struct gzr {
	gzFile gz;
	FILE* f;
	BZFILE* bz;
	int more;  /* bz is a follow-on stream that has not produced data yet */
};

#ifdef __cplusplus
extern "C" {
#endif

/* Report a decompression error to stderr and abort. gzread's own error
 * messages omit errno detail for Z_ERRNO, so fall back to a generic message. */
static void fatal_gz_error(gzFile gz) {
	int errnum = 0;
	const char* msg = gzerror(gz, &errnum);
	fprintf(stderr,
		"Error: failed to read/decompress input (%s)\n",
		(msg != NULL && *msg) ? msg : "unknown zlib error");
	exit(1);
}

static void fatal_bz_error(int err) {
	fprintf(stderr,
		"Error: failed to read/decompress bzip2 input (%s)\n",
		err == BZ_UNEXPECTED_EOF ? "unexpected end of file" :
		err == BZ_IO_ERROR ? "read error" : "invalid compressed data");
	exit(1);
}

/* True if the first bytes are a bzip2 header: "BZh", a block size '1'-'9',
 * then the 48-bit magic of the first block (or of the end of an empty
 * stream). FASTQ/FASTA files start with '@'/'>', so only a tab/qseq/raw
 * read file whose first line begins with exactly "BZh91AY&SY" (or the same
 * with block size 1-8) could be mistaken for bzip2. */
static int is_bzip2(const unsigned char* b, size_t n) {
	static const unsigned char block[6] = { 0x31, 0x41, 0x59, 0x26, 0x53, 0x59 };
	static const unsigned char end[6] = { 0x17, 0x72, 0x45, 0x38, 0x50, 0x90 };
	return n >= 10 && b[0] == 'B' && b[1] == 'Z' && b[2] == 'h' &&
		b[3] >= '1' && b[3] <= '9' &&
		(memcmp(b + 4, block, 6) == 0 || memcmp(b + 4, end, 6) == 0);
}

/* Wrap fd (taking ownership) for zlib reading; NULL on failure. */
static void* open_gz(int fd) {
	gzFile gz = gzdopen(fd, "rb");
	if(gz == NULL) {
		gzr_closefd(fd); /* gzdopen did not take ownership on failure */
		return NULL;
	}
	/* Match FileBuf's 256 KiB chunking for throughput; harmless if it fails. */
	gzbuffer(gz, 256 * 1024);
	struct gzr* h = (struct gzr*)calloc(1, sizeof(struct gzr));
	if(h == NULL) {
		gzclose(gz);
		return NULL;
	}
	h->gz = gz;
	return h;
}

void* gzr_open(const char* path) {
	int fd = gzr_openfd(path);
	if(fd < 0) {
		return NULL;
	}
	/* Sniff for bzip2 only when the file can be rewound afterwards; a pipe
	 * (e.g. a FIFO) goes straight to zlib, as before, so no bytes are lost. */
	if(gzr_lseek(fd, 0, SEEK_CUR) == 0) {
		unsigned char head[10];
		size_t n = 0;
		while(n < sizeof(head)) {
			int r = (int)gzr_readfd(fd, head + n, (unsigned)(sizeof(head) - n));
			if(r <= 0) break;
			n += (size_t)r;
		}
		if(gzr_lseek(fd, 0, SEEK_SET) != 0) {
			gzr_closefd(fd);
			return NULL;
		}
		if(is_bzip2(head, n)) {
			struct gzr* h = (struct gzr*)calloc(1, sizeof(struct gzr));
			FILE* f = (h != NULL) ? fdopen(fd, "rb") : NULL;
			int err = BZ_OK;
			BZFILE* bz = (f != NULL) ? BZ2_bzReadOpen(&err, f, 0, 0, NULL, 0) : NULL;
			if(bz == NULL || err != BZ_OK) {
				if(bz != NULL) BZ2_bzReadClose(&err, bz);
				if(f != NULL) fclose(f); else gzr_closefd(fd);
				free(h);
				return NULL;
			}
			h->f = f;
			h->bz = bz;
			return h;
		}
	}
	return open_gz(fd);
}

void* gzr_dopen(int fd) {
	/* dup so gzclose() does not close the caller's fd (e.g. real stdin). */
	int fd2 = gzr_dup(fd);
	if(fd2 < 0) {
		return NULL;
	}
	return open_gz(fd2);
}

/* Read up to n bytes of bzip2 data, continuing through concatenated streams
 * (as `bzip2 -dc` does). Bytes after the last stream that do not start
 * another one are ignored, also as `bzip2 -dc` does; any other error (bad
 * data, a truncated stream) is fatal. */
static size_t bz_read(struct gzr* h, char* buf, size_t n) {
	size_t got = 0;
	while(got < n && h->bz != NULL) {
		size_t want = n - got;
		int err = BZ_OK;
		int ret = BZ2_bzRead(&err, h->bz, buf + got, want > (1u << 30) ? (1 << 30) : (int)want);
		if(err == BZ_OK) {
			got += (size_t)ret;
			h->more = 0;
			continue;
		}
		if(err == BZ_DATA_ERROR_MAGIC && h->more) {
			BZ2_bzReadClose(&err, h->bz); /* trailing garbage */
			h->bz = NULL;
			break;
		}
		if(err != BZ_STREAM_END) {
			fatal_bz_error(err);
		}
		got += (size_t)ret;
		void* unused = NULL;
		int nunused = 0;
		char next[BZ_MAX_UNUSED];
		BZ2_bzReadGetUnused(&err, h->bz, &unused, &nunused);
		memcpy(next, unused, (size_t)nunused);
		BZ2_bzReadClose(&err, h->bz);
		h->bz = NULL;
		int c = (nunused > 0) ? EOF : getc(h->f);
		if(c != EOF) ungetc(c, h->f);
		if(nunused > 0 || c != EOF) {
			h->bz = BZ2_bzReadOpen(&err, h->f, 0, 0, next, nunused);
			if(err != BZ_OK) {
				fatal_bz_error(err);
			}
			h->more = 1;
		}
	}
	return got;
}

size_t gzr_read(void* hv, char* buf, size_t n) {
	if(hv == NULL) return 0;
	struct gzr* h = (struct gzr*)hv;
	if(h->f != NULL) return bz_read(h, buf, n);
	gzFile gz = h->gz;
	/* Fill the whole buffer, looping until it is full or gzread signals a
	 * definitive end-of-stream (a 0 return). We cannot stop at the first
	 * short read: zlib only discovers a truncated stream on the *next*
	 * gzread (the one that returns 0 with a negative errnum), but the caller
	 * (FileBuf) treats any short read as EOF and never calls back -- so a
	 * single short read carrying the whole truncated payload would slip
	 * through as a clean, exit-0 run with too few reads. Forcing the
	 * terminating read here surfaces the error. All zlib error codes are
	 * negative; Z_OK(0)/Z_STREAM_END(1) are not errors. */
	size_t got = 0;
	while(got < n) {
		int ret = gzread(gz, buf + got, (unsigned)(n - got));
		if(ret < 0) {
			fatal_gz_error(gz);
		}
		/* Check the error state after EVERY read, before acting on a 0 return.
		 * A truncated stream is reported at different moments across zlib
		 * builds: macOS/Linux flag it on the follow-up ret==0 read, while the
		 * Windows (MSYS2) build flags it on the SAME partial ret>0 read and
		 * then CLEARS it before the ret==0 read. Checking here catches both;
		 * a clean read never leaves a negative errnum. */
		int errnum = 0;
		gzerror(gz, &errnum);
		if(errnum < 0) {
			fatal_gz_error(gz);
		}
		if(ret == 0) {
			break; /* clean end of stream */
		}
		got += (size_t)ret;
	}
	return got;
}

void gzr_rewind(void* hv) {
	if(hv == NULL) return;
	struct gzr* h = (struct gzr*)hv;
	if(h->f == NULL) {
		gzrewind(h->gz);
		return;
	}
	int err = BZ_OK;
	if(h->bz != NULL) BZ2_bzReadClose(&err, h->bz);
	rewind(h->f);
	h->bz = BZ2_bzReadOpen(&err, h->f, 0, 0, NULL, 0);
	if(err != BZ_OK) {
		fatal_bz_error(err);
	}
	h->more = 0;
}

void gzr_close(void* hv) {
	if(hv == NULL) return;
	struct gzr* h = (struct gzr*)hv;
	if(h->f != NULL) {
		int err = BZ_OK;
		if(h->bz != NULL) BZ2_bzReadClose(&err, h->bz);
		fclose(h->f);
	} else {
		gzclose(h->gz);
	}
	free(h);
}

#ifdef __cplusplus
} /* extern "C" */
#endif
