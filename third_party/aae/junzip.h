/**
 * JUnzip library by Joonas Pihlajamaa (firstname.lastname@iki.fi).
 * Released into public domain. https://github.com/jokkebk/JUnzip
 */

#ifndef __JUNZIP_H
#define __JUNZIP_H

#include <stdint.h>

// Enable compiling without Zlib as well
// (no compression support, only "store")
#define Z_OK 0
#define Z_ERRNO -1

#include "puff.h"

// If you don't have stdint.h, the following two lines should work for most 32/64 bit systems
// typedef unsigned int uint32_t;
// typedef unsigned short uint16_t;

#define JZHOUR(t) ((t)>>11)
#define JZMINUTE(t) (((t)>>5) & 63)
#define JZSECOND(t) (((t) & 31) * 2)
#define JZTIME(h,m,s) (((h)<<11) + ((m)<<5) + (s)/2)

#define JZYEAR(t) (((t)>>9) + 1980)
#define JZMONTH(t) (((t)>>5) & 15)
#define JZDAY(t) ((t) & 31)
#define JZDATE(y,m,d) ((((y)-1980)<<9) + ((m)<<5) + (d))


typedef struct JZFile JZFile;

struct JZFile {
    size_t (*read)(JZFile *file, void *buf, size_t size);
    size_t (*tell)(JZFile *file);
    int (*seek)(JZFile *file, size_t offset, int whence);
    int (*error)(JZFile *file);
    void (*close)(JZFile *file);
    /* OPTIONAL: if the zip already lives in memory, returns a pointer to the `n` bytes
     * under the current position and advances. It allows decompressing WITHOUT copying them
     * into an intermediate buffer. Zero when that is not possible (reading from a file). */
    const unsigned char *(*direct)(JZFile *file, unsigned long n);
};


/* On bare metal the pointer is a `void *` and the functions that use it are excluded by
 * junzip.c's guard. This used to be solved with a `typedef void FILE` in romzip.h, and it
 * collided as soon as any header in the tree pulled in a real stdio.h. */
#if !defined(ROMZIP_NO_STDIO) && !defined(RA_ROMZIP_NO_STDIO)
#include <stdio.h>   /* FILE: this header names it, so this header brings it in */
#endif

typedef struct {
    JZFile handle;
#if defined(ROMZIP_NO_STDIO) || defined(RA_ROMZIP_NO_STDIO)
    void *fp;          /* no stdio: the field exists so the struct layout is unchanged */
#else
    FILE *fp;
#endif
} StdioJZFile;


#if !defined(ROMZIP_NO_STDIO) && !defined(RA_ROMZIP_NO_STDIO)
void jzfile_from_stdio_file(FILE *fp, StdioJZFile *handle);
#endif


typedef struct __attribute__ ((__packed__)) {
    uint32_t signature; // 0x04034B50
    uint16_t versionNeededToExtract; // unsupported
    uint16_t generalPurposeBitFlag; // unsupported
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t fileNameLength;
    uint16_t extraFieldLength; // unsupported
} JZLocalFileHeader;

typedef struct __attribute__ ((__packed__)) {
    uint32_t signature; // 0x02014B50
    uint16_t versionMadeBy; // unsupported
    uint16_t versionNeededToExtract; // unsupported
    uint16_t generalPurposeBitFlag; // unsupported
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t fileNameLength;
    uint16_t extraFieldLength; // unsupported
    uint16_t fileCommentLength; // unsupported
    uint16_t diskNumberStart; // unsupported
    uint16_t internalFileAttributes; // unsupported
    uint32_t externalFileAttributes; // unsupported
    uint32_t relativeOffsetOflocalHeader;
} JZGlobalFileHeader;

typedef struct __attribute__ ((__packed__)) {
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t offset;
} JZFileHeader;

typedef struct __attribute__ ((__packed__)) {
    uint32_t signature; // 0x06054b50
    uint16_t diskNumber; // unsupported
    uint16_t centralDirectoryDiskNumber; // unsupported
    uint16_t numEntriesThisDisk; // unsupported
    uint16_t numEntries;
    uint32_t centralDirectorySize;
    uint32_t centralDirectoryOffset;
    uint16_t zipCommentLength;
    // Followed by .ZIP file comment (variable size)
} JZEndRecord;

// Callback prototype for central and local file record reading functions
typedef int (*JZRecordCallback)(JZFile *zip, int index, JZFileHeader *header,
        char *filename, void *user_data);

/* This only serves to locate the zip's end record: the TAIL of the file is read and the
 * signature is searched for backwards. The record is 22 bytes plus the comment, and across
 * the 47 romsets the longest comment is 22 bytes (measured). 4 KB leaves three orders of
 * magnitude of margin over what was observed and saves 60 KB of RAM in EVERY game. The
 * format's theoretical limit is a 64 KB comment; if one ever turned up, jzReadEndRecord
 * returns an error and the game says so — nothing is corrupted silently.
 *
 * Re-measure it with:
 *   python3 -c "import zipfile,glob,os,sys;d=os.path.expanduser(sys.argv[1]);
 *     print(max(len(zipfile.ZipFile(z).comment) for z in glob.glob(d+'/'+chr(42)+'.zip')))" \
 *     ~/projects/vectrex-arcade-private/arcade/roms
 */
#define JZ_BUFFER_SIZE 2048

// Read ZIP file end record. Will move within file.
int jzReadEndRecord(JZFile *zip, JZEndRecord *endRecord);

// Read ZIP file global directory. Will move within file.
// Callback is called for each record, until callback returns zero
int jzReadCentralDirectory(JZFile *zip, JZEndRecord *endRecord,
        JZRecordCallback callback, void *user_data);

// Read local ZIP file header. Silent on errors so optimistic reading possible.
int jzReadLocalFileHeader(JZFile *zip, JZFileHeader *header,
        char *filename, int len);

// Same as above but returns the full raw header
int jzReadLocalFileHeaderRaw(JZFile *zip, JZLocalFileHeader *header,
        char *filename, int len);

// Read data from file stream, described by header, to preallocated buffer
// Return value is zlib coded, e.g. Z_OK, or error code
int jzReadData(JZFile *zip, JZFileHeader *header, void *buffer);

int loadFromZip(char *zipFile, char *name, unsigned char* block);

#endif
