#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <sys/mman.h>
#include <err.h>
#include <time.h>

#include "util.h"
#include "vfat.h"

#define assertf(A, M, ...) if(!(A)) { DEBUG_PRINT(M, ##__VA_ARGS__); \
        assert(A); }


uintptr_t page_floor(uintptr_t offset) {
    size_t pagesize = sysconf(_SC_PAGESIZE);
    assert(offset>0);
    return (offset / pagesize) * pagesize;
}

uintptr_t page_ceil(uintptr_t offset) {
    size_t pagesize = sysconf(_SC_PAGESIZE);
    assert(offset>0);
    return ((offset + pagesize - 1) / pagesize) * pagesize;
}

// mmap file content at given offset
// use unmap to release the mapping
void* mmap_file(int fd, off_t offset, size_t size)
{
    off_t offset_end = offset + size;
    assert(offset >= 0);
    assert(offset_end >= offset); // No overflow

    uintptr_t end = page_ceil(offset_end);
    uintptr_t start = page_floor(offset);

    uintptr_t len = end - start;
    void* buf = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, start);

    if (buf == MAP_FAILED)
        err(1, "mmap failed");

    return ((void *)((uintptr_t)buf + (offset - start)));
}

// buf: buffer returned by mmap_file()
// size: same size as supplied to the mmap_file()
void unmap(void* buf, size_t size)
{
    assert(buf);
    uintptr_t end = page_ceil((uintptr_t)buf + size);
    uintptr_t start = page_floor((uintptr_t)buf);
    assert(end > (uintptr_t)buf);
    size_t len = end - start;
    if (munmap((void*)start, len) < 0)
        err(1, "munmap failed");
}

void clear_time(struct tm *time)
{
    assert(time);
    memset(time, 0, sizeof(*time));
}

void vfat_parse_date(const uint16_t date, struct tm *out)
{
    out->tm_mday = (date & 0xF);
    out->tm_mon = (date >> 5) & 0b1111;
    out->tm_year = date >> 9;

    DEBUG_PRINT("Date: %d/%d/%d\n", out->tm_mday, out->tm_mon,
                out->tm_year + 1980);

    assertf(out->tm_mday > 0 && out->tm_mday < 32, "Invalid day: %d\n",
            out->tm_mday);
    assertf(out->tm_mon > 0 && out->tm_mon < 13, "Invalid month: %d\n",
           out->tm_mon);
}

void vfat_parse_time(const uint16_t time, struct tm *out)
{
    out->tm_hour = (time >> 11);
    out->tm_min = (time >> 5) & 0b111111;
    out->tm_sec = (time & 0xF) * 2;

    DEBUG_PRINT("Time: %d:%d:%d\n", out->tm_hour, out->tm_min, out->tm_sec);

    assertf(out->tm_hour >= 0 && out->tm_hour < 24, "Invalid hours: %d\n",
            out->tm_hour);
    assertf(out->tm_sec >= 0 && out->tm_sec < 60, "Invalid seconds: %d\n",
            out->tm_sec);
    assertf(out->tm_min >= 0 && out->tm_min < 60, "Invalid minutes: %d\n",
            out->tm_min);
}

void vfat_parse_timestamp(const struct fat32_direntry *dir, struct stat *out)
{
    assert(dir);
    assert(out);

    struct tm atime;
    struct tm mtime;
    struct tm ctime;

    clear_time(&atime);
    clear_time(&mtime);
    clear_time(&ctime);

    vfat_parse_date(dir->atime_date, &atime);
    vfat_parse_date(dir->ctime_date, &ctime);
    vfat_parse_time(dir->ctime_time, &ctime);
    vfat_parse_date(dir->mtime_date, &mtime);
    vfat_parse_time(dir->mtime_time, &mtime);


    out->st_ctime = mktime(&ctime);
    out->st_mtime = mktime(&mtime);
    out->st_atime = mktime(&atime);
}
