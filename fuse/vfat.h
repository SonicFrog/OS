// vim: noet:ts=4:sts=4:sw=4:et
#ifndef VFAT_H
#define VFAT_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <iconv.h>
#include <errno.h>

#define DEBUG_PRINT(format, ...) printf("%s:%s:%d: " format, __FILE__,  \
                                        __func__, __LINE__,  ##__VA_ARGS__)

iconv_t iconv_utf16;

#define FAT32_MIN_CLUSTERS_COUNT 65525
#define FAT32_SIGNATURE 0xAA55
#define FAT32_END_OF_CHAIN 0xFFFFFFF
#define FAT32_DIRENTRY_SIZE 32
#define FAT32_UNUSED_ENTRY 0xE5

#define NAME_MAX 255

#define VFAT_LFN_NAME1_SIZE (5 * 2)
#define VFAT_LFN_NAME2_SIZE (6 * 2)
#define VFAT_LFN_NAME3_SIZE (2 * 2)
#define VFAT_LFN_SIZE VFAT_LFN_NAME1_SIZE + VFAT_LFN_NAME2_SIZE + \
    VFAT_LFN_NAME3_SIZE

#define PRINT_TAB(i, tab, size) for(i = 0; i < size; i++) { \
        printf("%02X ", tab[i]); } printf("\n");

// Boot sector
struct fat_boot_header {
    /* General */
    /* 0*/  uint8_t  jmp_boot[3];
    /* 3*/  char     oemname[8];
    /*11*/  uint16_t bytes_per_sector;
    /*13*/  uint8_t  sectors_per_cluster;
    /*14*/  uint16_t reserved_sectors;
    /*16*/  uint8_t  fat_count;
    /*17*/  uint16_t root_max_entries;
    /*19*/  uint16_t total_sectors_small;
    /*21*/  uint8_t  media_info;
    /*22*/  uint16_t sectors_per_fat_small;
    /*24*/  uint16_t sectors_per_track;
    /*26*/  uint16_t head_count;
    /*28*/  uint32_t fs_offset;
    /*32*/  uint32_t total_sectors;
    /* FAT32-only */
    /*36*/  uint32_t sectors_per_fat;
    /*40*/  uint16_t fat_flags;
    /*42*/  uint16_t version;
    /*44*/  uint32_t root_cluster;
    /*48*/  uint16_t fsinfo_sector;
    /*50*/  uint16_t backup_sector;
    /*52*/  uint8_t  reserved2[12];
    /*64*/  uint8_t  drive_number;
    /*65*/  uint8_t  reserved3;
    /*66*/  uint8_t  ext_sig;
    /*67*/  uint32_t serial;
    /*71*/  char     label[11];
    /*82*/  char     fat_name[8];
    /* Rest */
    /*90*/  char     executable_code[420];
    /*510*/ uint16_t signature;
} __attribute__ ((__packed__));


struct fat32_direntry {
    /* 0*/  union {
                struct {
                    char name[8];
                    char ext[3];
                };
                char nameext[11];
            };
    /*11*/  uint8_t  attr;
    /*12*/  uint8_t  res;
    /*13*/  uint8_t  ctime_ms;
    /*14*/  uint16_t ctime_time;
    /*16*/  uint16_t ctime_date;
    /*18*/  uint16_t atime_date;
    /*20*/  uint16_t cluster_hi;
    /*22*/  uint16_t mtime_time;
    /*24*/  uint16_t mtime_date;
    /*26*/  uint16_t cluster_lo;
    /*28*/  uint32_t size;
} __attribute__ ((__packed__));

#define VFAT_ATTR_DIR   0x10
#define VFAT_ATTR_LFN   0xf
#define VFAT_ATTR_INVAL (0x80|0x40|0x08)

struct fat32_direntry_long {
    /* 0*/  uint8_t  seq;
    /* 1*/  uint16_t name1[5];
    /*11*/  uint8_t  attr;
    /*12*/  uint8_t  type;
    /*13*/  uint8_t  csum;
    /*14*/  uint16_t name2[6];
    /*26*/  uint16_t reserved2;
    /*28*/  uint16_t name3[2];
} __attribute__ ((__packed__));

#define VFAT_LFN_SEQ_START      0x40
#define VFAT_LFN_SEQ_DELETED    0x80
#define VFAT_LFN_SEQ_MASK       0x3f


// A kitchen sink for all important data about filesystem
struct vfat_data {
    const char* dev;
    int         fd;
    uid_t mount_uid;
    gid_t mount_gid;
    time_t mount_time;
    /* TODO: add your code here */
    size_t      fat_entries;
    size_t      cluster_count;
    off_t       cluster_begin_offset;
    size_t      direntry_per_cluster;
    size_t      bytes_per_sector;
    size_t      sectors_per_cluster;
    size_t      reserved_sectors;
    size_t      sectors_per_fat;
    size_t      cluster_size;
    off_t       fat_begin_offset;
    size_t      fat_size;
    struct stat root_inode;
    uint32_t*   fat; // use util::mmap_file() to map this directly into the memory
};

struct vfat_data vfat_info;

/// FOR debugfs
uint32_t vfat_next_cluster(unsigned int c);
int vfat_resolve(const char *path, struct stat *st);
int vfat_fuse_getattr(const char *path, struct stat *st);
inline int read_direntry_at(int fd, struct fat32_direntry *dest, off_t offs);
inline bool is_lfn_entry_begin(const struct fat32_direntry *dir);
///

#define IS_UNUSED(dir) ((dir).name[0] == 0x5E)

#define IS_LFN_ENTRY(dir) (((dir)->attr & VFAT_ATTR_LFN) ==  VFAT_ATTR_LFN)

#define HAS_MORE_DIRS(dir) (*((uint8_t *) dir) != 0x00)

#define IS_DIRECTORY(dir) ((dir)->attr & VFAT_ATTR_DIR)


#define DIRENTRY_SIZE sizeof(struct fat32_direntry)

#endif
