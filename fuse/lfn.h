#ifndef __DEF__LFN_VFAT
#define __DEF__LFN_VFAT

#include <stdint.h>
#include <sys/types.h>

#include "vfat.h"

int parse_long_name(uint16_t * dest);

size_t copy_long_name(uint16_t *dest, struct fat32_direntry_long* dir);
void shift_long_name(uint16_t *dest, size_t size);

#endif
