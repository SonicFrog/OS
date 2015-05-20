#ifndef H_UTIL
#define H_UTIL

#include <stdint.h>
#include <sys/types.h>
#include <wchar.h>

#include "vfat.h"

void* mmap_file(int fd, off_t offset, size_t size);
void unmap(void* buf, size_t size);

void vfat_parse_timestamp(const struct fat32_direntry *dir, struct stat *out);
void vfat_parse_time(const uint16_t time, struct tm *out);
void vfat_parse_date(const uint16_t date, struct tm *out);

#define MIN(a, b) ((a) < (b) ? b : a)

#endif
