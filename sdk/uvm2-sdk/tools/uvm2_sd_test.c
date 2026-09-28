/* Host harness for uvm2_sd.c: the very same file-system code, pointed at a disk IMAGE instead
 * of the card, so writing can be tested without risking anybody's card. One call per run:
 *
 *     uvm2_sd_test IMG read      PATH OUT          whole file -> OUT
 *     uvm2_sd_test IMG readfrom  PATH OFF MAX OUT  a chunk    -> OUT
 *     uvm2_sd_test IMG write     PATH IN           IN -> PATH (any size)
 *     uvm2_sd_test IMG create    PATH TEXT         512-byte text file
 *     uvm2_sd_test IMG overwrite PATH TEXT         first sector of an existing file
 *
 * It prints `ok=<0|1> err=<uvm2_sd_error> n=<bytes> fs=<fs_type>` and exits 0 whatever the
 * outcome; tools/uvm2_sd_test.sh decides what was expected. Build:
 *
 *     cc -O2 -DUVM2_SD_HOST -I. -I../../third_party/fatfs -o /tmp/uvm2_sd_test \
 *        tools/uvm2_sd_test.c uvm2_sd.c \
 *        ../../third_party/fatfs/ff.c ../../third_party/fatfs/ffunicode.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uvm2_sd.h"

static FILE *img;

int uvm2_host_read(uint32_t lba, unsigned char *b)
{
    return fseek(img, (long)lba * 512, SEEK_SET) == 0 && fread(b, 1, 512, img) == 512;
}
int uvm2_host_write(uint32_t lba, const unsigned char *b)
{
    return fseek(img, (long)lba * 512, SEEK_SET) == 0 && fwrite(b, 1, 512, img) == 512;
}

static unsigned char buf[16u << 20];

static void save(const char *out, uint32_t n)
{
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(buf, 1, n, f) != n) { perror(out); exit(2); }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: see the top of uvm2_sd_test.c\n"); return 2; }
    img = fopen(argv[1], "r+b");
    if (!img) { perror(argv[1]); return 2; }
    const char *op = argv[2], *path = argv[3];
    uint32_t n = 0;
    int ok;

    if (!strcmp(op, "read") && argc == 5) {
        n = uvm2_sd_read(path, buf, sizeof buf);
        ok = n != 0;
        if (ok) save(argv[4], n);
    } else if (!strcmp(op, "readfrom") && argc == 7) {
        uint32_t max = (uint32_t)strtoul(argv[5], 0, 0);
        if (max > sizeof buf) max = sizeof buf;
        n = uvm2_sd_read_from(path, buf, max, (uint32_t)strtoul(argv[4], 0, 0));
        ok = uvm2_sd_error == UVM2_SD_OK;
        if (ok) save(argv[6], n);
    } else if (!strcmp(op, "write") && argc == 5) {
        FILE *f = fopen(argv[4], "rb");
        if (!f) { perror(argv[4]); return 2; }
        n = (uint32_t)fread(buf, 1, sizeof buf, f);
        fclose(f);
        ok = uvm2_sd_write(path, buf, n);
    } else if (!strcmp(op, "create") && argc == 5) {
        n = (uint32_t)strlen(argv[4]);
        ok = uvm2_sd_create(path, (const unsigned char *)argv[4], n);
    } else if (!strcmp(op, "overwrite") && argc == 5) {
        n = (uint32_t)strlen(argv[4]);
        ok = uvm2_sd_overwrite(path, (const unsigned char *)argv[4], n);
    } else {
        fprintf(stderr, "bad arguments; see the top of uvm2_sd_test.c\n");
        return 2;
    }
    printf("ok=%d err=%d n=%u fs=%u\n", ok, uvm2_sd_error, n, uvm2_sd_diag.fs_type);
    fclose(img);
    return 0;
}
