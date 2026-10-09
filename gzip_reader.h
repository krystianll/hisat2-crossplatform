/*
 * gzip_reader.h
 *
 * Thin zlib wrapper for streaming (optionally gzip-compressed) read input.
 *
 * Isolated in its own translation unit so that <zlib.h> never leaks into
 * filebuf.h (or anything that includes it): callers see only an opaque
 * handle (void*) and a handful of C-linkage functions.  zlib's gz* API
 * transparently reads BOTH gzip-compressed and plain files, so a single
 * code path replaces the previous "decompress to a temp file first" launcher
 * step -- no multi-GB FASTQ is ever written to disk.
 *
 * Added for the deeptoolsr bundle; upstream hisat2 has no native gzip
 * support in its read parser (unlike its sibling bowtie2).  The feature is
 * useful on every platform, so it is intentionally NOT #ifdef-guarded.
 */

#ifndef GZIP_READER_H_
#define GZIP_READER_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a file path for reading. Handles gzip and plain files transparently.
 * Returns an opaque handle, or NULL if the file could not be opened (the
 * caller distinguishes this from EOF, exactly as a failed fopen did). */
void* gzr_open(const char* path);

/* Wrap an already-open file descriptor (e.g. stdin) for reading. Handles gzip
 * and plain input transparently, so a piped `.gz` on stdin works. The fd is
 * dup'd, so closing the handle does not close the caller's original fd. On
 * Windows the fd must already be in binary mode (see _setmode in main()).
 * Returns an opaque handle, or NULL on failure. */
void* gzr_dopen(int fd);

/* Read up to n bytes of *uncompressed* data into buf. Returns the number of
 * bytes read; 0 means clean end-of-stream. A decompression error (truncated
 * stream, bad CRC, invalid data) is fatal: it is reported to stderr and the
 * process exits non-zero rather than being silently treated as EOF. */
size_t gzr_read(void* h, char* buf, size_t n);

/* Rewind the stream to the beginning (used by FileBuf::reset()). */
void gzr_rewind(void* h);

/* Close the handle and release its resources. Safe on NULL. */
void gzr_close(void* h);

#ifdef __cplusplus
}
#endif

#endif /*ndef GZIP_READER_H_*/
