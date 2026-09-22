#ifndef _AAE_STDIO_H
#define _AAE_STDIO_H
typedef void FILE;            /* logging is a no-op on the cart */
extern FILE *stderr, *stdout;
int printf(const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);

/* File I/O: the cart has no filesystem here (ROMs are embedded, no save states).
 * These are declared so acommon.c's save/load-state + file loaders compile;
 * they are no-op stubs (fopen returns NULL) and --gc-sections drops the unused
 * callers. */
#include <stddef.h>
FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t n, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *f);
int   fseek(FILE *f, long off, int whence);
long  ftell(FILE *f);
int   fputc(int c, FILE *f);
int   fgetc(FILE *f);
#endif
