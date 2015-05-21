#ifndef __DEF__LFN_VFAT
#define __DEF__LFN_VFAT

#include <stdint.h>
#include <sys/types.h>

#include "vfat.h"

void copy_long_name(char *dest, const struct fat32_direntry_long* dir);
int read_lfn(const struct fat32_direntry_long *dir);
int get_lfn(char *output);
uint8_t calc_csum(const uint8_t shrt_name[11]);

#endif
