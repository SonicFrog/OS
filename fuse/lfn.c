#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "vfat.h"
#include "lfn.h"


int parse_long_name(uint16_t *dest)
{
    assert(dest);


    return 0;
}

void clean_ucs_string(uint16_t *str, size_t len)
{
    uint16_t *pos;
    uint16_t needle = 0xFFFF;

    while((pos = memmem(str, len, &needle, sizeof(uint16_t))) != NULL)
    {
        *pos = 0x0;
    }
}

void clean_long_entry(struct fat32_direntry_long *dir)
{
    clean_ucs_string(dir->name1, VFAT_LFN_NAME1_SIZE);
    clean_ucs_string(dir->name2, VFAT_LFN_NAME2_SIZE);
    clean_ucs_string(dir->name3, VFAT_LFN_NAME3_SIZE);

    DEBUG_PRINT("Cleaned UCS: %s%s%s\n", (char *) dir->name1,
                (char *) dir->name2, (char *) dir->name3);
}

size_t copy_long_name(uint16_t *dest, struct fat32_direntry_long* dir)
{
    assert(dir);
    assert(dest);

    size_t len = 0;

    clean_long_entry(dir);

    memcpy(dest, dir->name1, VFAT_LFN_NAME1_SIZE);
    dest += VFAT_LFN_NAME1_SIZE;
    len += strlen((char *) dir->name1);

    if (len < VFAT_LFN_NAME1_SIZE)
    {
        return len;
    }

    memcpy(dest, dir->name2, VFAT_LFN_NAME2_SIZE);
    dest += VFAT_LFN_NAME2_SIZE;
    len += strlen((char *) dir->name2);

    if (len < VFAT_LFN_NAME2_SIZE + VFAT_LFN_NAME1_SIZE)
    {
        return len;
    }

    memcpy(dest, dir->name3, VFAT_LFN_NAME3_SIZE);
    len += strlen((char *) dir->name3);

    return len;
}

void shift_long_name(uint16_t* name, size_t current_len)
{
    int i;

    assert(name);

    for (i = current_len - 1; i >= 0; i--)
    {
        name[i + VFAT_LFN_SIZE] = name[i];
    }
}
