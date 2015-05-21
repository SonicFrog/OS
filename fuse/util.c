#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>
#include <sys/mman.h>
#include <err.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>

#include "util.h"
#include "vfat.h"

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

inline bool is_valid_direntry(struct fat32_direntry *dir)
{
    uint8_t dir0 = dir->name[0];

    if (dir->attr & VFAT_ATTR_INVAL)
        return false;

    if (dir0 <= 0x20)
        return false;

    switch (dir0)
    {
        //Illegal values for dirname[0] from MS spec
    case 0x22:
    case 0x2A:
    case 0x2B:
    case 0x2C:
    case 0x2E:
    case 0x2F:
    case 0x3A:
    case 0x3B:
    case 0x3C:
    case 0x3D:
    case 0x3E:
    case 0x3F:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x7C:
    case 0xE5:
        return false;
    }

    return true;
}

void vfat_parse_date(const uint16_t date, struct tm *out)
{
    out->tm_mday = (date & 0xF);
    out->tm_mon = ((date >> 5) & 0b1111) - 1;
    out->tm_year = (date >> 9) + 80;

    assertf(out->tm_mday > 0 && out->tm_mday < 32, "Invalid day: %d\n",
            out->tm_mday);
    assertf(out->tm_mon > 0 && out->tm_mon < 13, "Invalid month: %d\n",
            out->tm_mon);
}

void vfat_parse_time(const uint16_t time, const uint8_t ctime_ms, struct tm *out)
{
    out->tm_hour = (time >> 11);
    out->tm_min = (time >> 5) & 0b111111;
    out->tm_sec = (time & 0x1F) * 2;
    out->tm_sec += ceil(ctime_ms / 100);

    assertf(ctime_ms <= 199, "Invalid ctime_ms: %d\n", ctime_ms);
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
    vfat_parse_time(dir->ctime_time, dir->ctime_ms, &ctime);
    vfat_parse_date(dir->mtime_date, &mtime);
    vfat_parse_time(dir->mtime_time, 0, &mtime);


    out->st_ctime = mktime(&ctime);
    out->st_mtime = mktime(&mtime);
    out->st_atime = mktime(&atime);
}
