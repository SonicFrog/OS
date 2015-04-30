// vim: noet:ts=4:sts=4:sw=4:et
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <assert.h>
#include <endian.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <iconv.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "vfat.h"
#include "util.h"
#include "debugfs.h"

#define DEBUG_PRINT(...) printf(__VA_ARGS)

iconv_t iconv_utf16;
char* DEBUGFS_PATH = "/.debug";


static const uint16_t bps_values[] = { 512, 1024, 2048, 4096 };
static const uint8_t spc_values[] = { 1, 2, 4, 16, 32, 64, 128 };


static bool check_value_in_table(const void *val, const void* table,
                                 const uint8_t size, const uint8_t tbl_sz)
{
    uint16_t i;

    for (i = 0; i < tbl_sz; i++)
    {
        if (memcmp(val, table + size * i, size) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool check_fat_version(const struct fat_boot_header *header,
                              struct vfat_data *data)
{
    bool ok = true;
    uint16_t root_dir_sectors;
    uint16_t fat_size;
    uint32_t data_sectors;
    uint16_t clusters_count;

    ok = check_value_in_table(&header->bytes_per_sector, bps_values,
                              sizeof(uint16_t), sizeof(bps_values) / sizeof(uint16_t));

    if (!ok)
    {
        return false;
    }

    ok = check_value_in_table(&header->sectors_per_cluster, spc_values,
                              sizeof(uint8_t), sizeof(spc_values) / sizeof(uint8_t));

    if (!ok)
    {
        return false;
    }

    root_dir_sectors = ((header->root_max_entries * 32) +
                        (header->bytes_per_sector -1)) / header->bytes_per_sector;

    if (header->sectors_per_fat_small != 0)
    {
        return false;
    }

    if (header->total_sectors_small != 0)
    {
        return 0;
    }

    data_sectors = header->total_sectors - header->reserved_sectors +
        (header->fat_count * header->sectors_per_fat);

    clusters_count = data_sectors / header->sectors_per_cluster;


    if (clusters_count < FAT32_MIN_CLUSTERS_COUNT)
    {
        return false;
    }

    data->cluster_size = header->sectors_per_cluster * header->bytes_per_sector;
    data->sectors_per_fat = header->sectors_per_fat;
    data->bytes_per_sector = header->bytes_per_sector;
    data->sectors_per_cluster = header->sectors_per_cluster;
    data->reserved_sectors = header->reserved_sectors;
    data->fat_size = data->sectors_per_fat * data->bytes_per_sector;
    data->direntry_per_cluster = data->cluster_size / sizeof(struct fat32_direntry);
    data->fat_begin_offset = data->reserved_sectors + FAT32_BOOT_HEADER_LEN;

    return true;
}

static void
vfat_init(const char *dev)
{
    struct fat_boot_header s;

    iconv_utf16 = iconv_open("utf-8", "utf-16"); // from utf-16 to utf-8
    // These are useful so that we can setup correct permissions in the mounted directories
    vfat_info.mount_uid = getuid();
    vfat_info.mount_gid = getgid();

    // Use mount time as mtime and ctime for the filesystem root entry (e.g. "/")
    vfat_info.mount_time = time(NULL);

    vfat_info.fd = open(dev, O_RDONLY);
    if (vfat_info.fd < 0)
        err(1, "open(%s)", dev);
    if (pread(vfat_info.fd, &s, sizeof(s), 0) != sizeof(s))
        err(1, "read super block");

    if (!check_fat_version(&s, &vfat_info))
    {
        close(vfat_info.fd);
        return;
    }
}

/* XXX add your code here */


int vfat_next_cluster(uint32_t c)
{
    /* TODO: Read FAT to actually get the next cluster */
    return 0xffffff; // no next cluster
}

int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t callback, void *callbackdata)
{
    struct stat st; // we can reuse same stat entry over and over again

    memset(&st, 0, sizeof(st));
    st.st_uid = vfat_info.mount_uid;
    st.st_gid = vfat_info.mount_gid;
    st.st_nlink = 1;

    /* XXX add your code here */
    return 0;
}


// Used by vfat_search_entry()
struct vfat_search_data {
    const char*  name;
    int          found;
    struct stat* st;
};


// You can use this in vfat_resolve as a callback function for vfat_readdir
// This way you can get the struct stat of the subdirectory/file.
int vfat_search_entry(void *data, const char *name, const struct stat *st, off_t offs)
{
    struct vfat_search_data *sd = data;

    if (strcmp(sd->name, name) != 0) return 0;

    sd->found = 1;
    *sd->st = *st;

    return 1;
}

/**
 * Fills in stat info for a file/directory given the path
 * @path full path to a file, directories separated by slash
 * @st file stat structure
 * @returns 0 iff operation completed succesfully -errno on error
 */
int vfat_resolve(const char *path, struct stat *st)
{
    /* TODO: Add your code here.
       You should tokenize the path (by slash separator) and then
       for each token search the directory for the file/dir with that name.
       You may find it useful to use following functions:
       - strtok to tokenize by slash. See manpage
       - vfat_readdir in conjuction with vfat_search_entry
    */
    int res = -ENOENT; // Not Found
    return res;
}

// Get file attributes
int vfat_fuse_getattr(const char *path, struct stat *st)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_getattr(path + strlen(DEBUGFS_PATH), st);
    } else {
        // Normal file
        return vfat_resolve(path, st);
    }
}

// Extended attributes useful for debugging
int vfat_fuse_getxattr(const char *path, const char* name, char* buf, size_t size)
{
    struct stat st;
    int ret = vfat_resolve(path, &st);
    if (ret != 0) return ret;
    if (strcmp(name, "debug.cluster") != 0) return -ENODATA;

    if (buf == NULL) {
        ret = snprintf(NULL, 0, "%u", (unsigned int) st.st_ino);
        if (ret < 0) err(1, "WTF?");
        return ret + 1;
    } else {
        ret = snprintf(buf, size, "%u", (unsigned int) st.st_ino);
        if (ret >= size) return -ERANGE;
        return ret;
    }
}

int vfat_fuse_readdir(
                      const char *path, void *callback_data,
                      fuse_fill_dir_t callback, off_t unused_offs, struct fuse_file_info *unused_fi)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_readdir(path + strlen(DEBUGFS_PATH), callback_data, callback, unused_offs, unused_fi);
    }
    /* TODO: Add your code here. You should reuse vfat_readdir and vfat_resolve functions
     */
    return 0;
}

int vfat_fuse_read(
                   const char *path, char *buf, size_t size, off_t offs,
                   struct fuse_file_info *unused)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs, unused);
    }
    /* TODO: Add your code here. Look at debugfs_fuse_read for example interaction.
     */
    return 0;
}

////////////// No need to modify anything below this point
int
vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
    if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
        vfat_info.dev = strdup(arg);
        return (0);
    }
    return (1);
}

struct fuse_operations vfat_available_ops = {
    .getattr = vfat_fuse_getattr,
    .getxattr = vfat_fuse_getxattr,
    .readdir = vfat_fuse_readdir,
    .read = vfat_fuse_read,
};

int main(int argc, char **argv)
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    fuse_opt_parse(&args, NULL, NULL, vfat_opt_args);

    if (!vfat_info.dev)
        errx(1, "missing file system parameter");

    vfat_init(vfat_info.dev);
    return (fuse_main(args.argc, args.argv, &vfat_available_ops, NULL));
}
