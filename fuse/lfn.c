#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <err.h>

#include "vfat.h"
#include "lfn.h"
#include "util.h"

static bool has_lfn = false;
static int lfn_entries_count = 0;
static char lfn_entries[NAME_MAX][NAME_MAX];

uint8_t calc_csum(uint8_t const short_name[11])
{
    short FcbNameLen;
    unsigned char Sum;
    Sum = 0;
    for (FcbNameLen = 11; FcbNameLen != 0; FcbNameLen--)
    {
        Sum = ((Sum & 1) ? 0x80 : 0) + (Sum >> 1) + *short_name++;
    }

    return (Sum);
}

int get_lfn(char *output)
{
    int res = -ENOENT;
    int i;

    if (has_lfn)
    {
        for (i = lfn_entries_count - 1; i >= 0; i--)
        {
            memcpy(output, lfn_entries[i], VFAT_LFN_SIZE);
            output += VFAT_LFN_SIZE;
        }
        res = 0;
        has_lfn = false;
        lfn_entries_count = 0;
    }

    return res;
}

void copy_long_name(char *dest, const struct fat32_direntry_long* dir)
{
    assert(dir);
    assert(dest);

    memcpy(dest, dir->name1, VFAT_LFN_NAME1_SIZE);
    dest += VFAT_LFN_NAME1_SIZE;

    memcpy(dest, dir->name2, VFAT_LFN_NAME2_SIZE);
    dest += VFAT_LFN_NAME2_SIZE;

    memcpy(dest, dir->name3, VFAT_LFN_NAME3_SIZE);
}

int read_lfn(const struct fat32_direntry_long *dir)
{
    size_t res = 0;
    char *source;
    char *source_saved;
    char *dest;
    char *dest_saved;
    size_t insize = VFAT_LFN_SIZE;
    size_t outsize = NAME_MAX;

    iconv_utf16 = iconv_open("utf-8", "utf-16");

    dest_saved = dest = calloc(outsize, sizeof(char));
    source_saved = source = calloc(insize, sizeof(char));

    has_lfn = true;

    copy_long_name(source, dir);

    DEBUG_PRINT("%s\n", source);
    DEBUG_PRINT("Iconv output size: %zd\n", outsize);
    DEBUG_PRINT("Iconv input size: %zd\n", insize);

    DEBUG_PRINT("Converting %zd bytes in UTF-8 using iconv...\n", insize);

    res = iconv(iconv_utf16, (char **) &source, &insize,
                (char **) &dest, &outsize);

    assertf(res != (size_t) -1, "Iconv failed to convert characters: %s\n",
            strerror(errno));

    DEBUG_PRINT("Iconv left %zd bytes\n", insize);

    memset(lfn_entries[lfn_entries_count], 0, NAME_MAX);
    memcpy(lfn_entries[lfn_entries_count++], dest_saved, NAME_MAX - insize);

    free(dest_saved);
    free(source_saved);

    return res;
}
