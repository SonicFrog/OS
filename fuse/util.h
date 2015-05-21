#ifndef H_UTIL
#define H_UTIL

#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>
#include <time.h>

#include "vfat.h"

void* mmap_file(int fd, off_t offset, size_t size);
void unmap(void* buf, size_t size);

bool is_valid_direntry(struct fat32_direntry *dir);

void vfat_parse_timestamp(const struct fat32_direntry *dir, struct stat *out);
void vfat_parse_time(const uint16_t time, const uint8_t ctime_ms,
                     struct tm *out);
void vfat_parse_date(const uint16_t date, struct tm *out);

#define assertf(A, M, ...) if(!(A)) { DEBUG_PRINT(M, ##__VA_ARGS__);    \
        assert(A); }

#define MIN(a, b) ((a) < (b) ? a : b)

#endif
