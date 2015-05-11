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
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "vfat.h"
#include "util.h"
#include "debugfs.h"

#define DEBUG_PRINT(...) printf(__VA_ARGS__)

iconv_t iconv_utf16;
char* DEBUGFS_PATH = "/.debug";


#define IS_LFN_ENTRY(attr) (attr & VFAT_ATTR_LFN)
#define IS_VALID_ENTRY(dir) *((uint8_t *) dir) != 0xE5  \
        && *((uint8_t *) dir) != 0x00
#define HAS_MORE_DIRS(dir) *((uint8_t *) dir) != 0x00


static const uint16_t bps_values[] = { 512, 1024, 2048, 4096 };
static const uint8_t spc_values[] = { 1, 2, 4, 8, 16, 32, 64, 128 };


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

static inline void fill_stat(const struct fat32_direntry *dir, struct stat *st)
{

}

static bool check_fat_version(const struct fat_boot_header *header,
                              struct vfat_data *data)
{
    bool ok = true;
    uint16_t root_dir_sectors;
    uint32_t data_sectors;

    DEBUG_PRINT("Checking FS for validity...\n");

    ok = check_value_in_table(&header->bytes_per_sector, bps_values,
                              sizeof(uint16_t), sizeof(bps_values) / sizeof(uint16_t));

    if (!ok)
    {
        return false;
    }

    DEBUG_PRINT("Bytes per sector is %d\n", header->bytes_per_sector);

    ok = check_value_in_table(&header->sectors_per_cluster, spc_values,
                              sizeof(uint8_t), sizeof(spc_values) / sizeof(uint8_t));

    if (!ok)
    {
        fprintf(stderr, "Invalid sectors per cluster value: %d\n",
                header->sectors_per_cluster);
        return false;
    }

    DEBUG_PRINT("Sectors per cluster is %d\n", header->sectors_per_cluster);

    root_dir_sectors = ((header->root_max_entries * 32) +
                        (header->bytes_per_sector -1)) / header->bytes_per_sector;

    if (root_dir_sectors != 0)
    {
        fprintf(stderr, "Root dir sectors is not 0!\n");
        return false;
    }

    if (header->sectors_per_fat_small != 0)
    {
        DEBUG_PRINT("Not a valid FAT32 filesystem!\n");
        return false;
    }

    if (header->total_sectors_small != 0)
    {
        DEBUG_PRINT("Not a valid FAT32 filesystem!\n");
        return false;
    }

    data_sectors = header->total_sectors - header->reserved_sectors +
        (header->fat_count * header->sectors_per_fat);

    DEBUG_PRINT("This fs contains %d data sectors\n", data_sectors);

    data->cluster_count = data_sectors / header->sectors_per_cluster;

    DEBUG_PRINT("Data clusters count %zd\n", data->cluster_count);

    if (data->cluster_count < FAT32_MIN_CLUSTERS_COUNT)
    {
        DEBUG_PRINT("Cluster count seems low: %zd\n", data->cluster_count);
    }

    if (header->signature != FAT32_SIGNATURE)
    {
        DEBUG_PRINT("Volume is not FAT32 formatted: bad signature %x\n",
                    header->signature);
        return false;
    }

    data->cluster_size = header->sectors_per_cluster * header->bytes_per_sector;
    data->sectors_per_fat = header->sectors_per_fat;
    data->bytes_per_sector = header->bytes_per_sector;
    data->sectors_per_cluster = header->sectors_per_cluster;
    data->reserved_sectors = header->reserved_sectors;
    data->fat_size = header->sectors_per_fat * header->bytes_per_sector;
    data->fat_entries = data->fat_size / sizeof(uint32_t);
    data->direntry_per_cluster = data->cluster_size / sizeof(struct fat32_direntry);
    data->fat_begin_offset = data->reserved_sectors * header->bytes_per_sector;
    data->cluster_begin_offset = data->fat_begin_offset + data->fat_size * header->fat_count;

    if (memchr(header->fat_name, '\0', 8))
        DEBUG_PRINT("Volume name: %s\n", header->fat_name);

    DEBUG_PRINT("OEM name: %s\n", header->oemname);
    DEBUG_PRINT("FAT begins at 0x%zx\n", data->fat_begin_offset);
    DEBUG_PRINT("%zd reserved sectors\n", data->reserved_sectors);
    DEBUG_PRINT("%zd fat entries\n", data->fat_entries);
    DEBUG_PRINT("First data cluster at 0x%zx\n", data->cluster_begin_offset);

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
    {
        err(1, "open(%s)", dev);
    }

    if (pread(vfat_info.fd, &s, sizeof(s), 0) != sizeof(s))
    {
        err(1, "read super block");
    }

    if (!check_fat_version(&s, &vfat_info))
    {
        close(vfat_info.fd);
        err(1, "invalid FS!");
    }

    vfat_info.root_inode.st_gid = vfat_info.mount_gid;
    vfat_info.root_inode.st_uid = vfat_info.mount_uid;
    vfat_info.root_inode.st_ino = 0;
    /* FIXME: Set the mode of the stat struct correctly */

    vfat_info.fat = mmap_file(vfat_info.fd, vfat_info.fat_begin_offset,
                              vfat_info.fat_size);

    if (vfat_info.fat == MAP_FAILED)
    {
        err(1, "Failed to mmap file");
    }
}

/* XXX add your code here */


uint32_t vfat_next_cluster(uint32_t c)
{
    uint32_t next_cluster_addr;

    if (c > vfat_info.fat_entries)
    {
        return 0xFFFFFFF;
    }

    next_cluster_addr = vfat_info.fat[c];

    DEBUG_PRINT("Cluster following n°%d is %x\n", c, next_cluster_addr);

    return next_cluster_addr;
}

int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t callback,
                 void *callbackdata)
{
    struct stat st; // we can reuse same stat entry over and over again
    uint32_t cluster_num = first_cluster;
    off_t offset = vfat_info.cluster_begin_offset +
        cluster_num * vfat_info.cluster_size;
    struct fat32_direntry dir;
    uint32_t dir_count = 0;

    memset(&st, 0, sizeof(st));
    st.st_uid = vfat_info.mount_uid;
    st.st_gid = vfat_info.mount_gid;
    st.st_nlink = 1;

    DEBUG_PRINT("Reading dir at %ud cluster", first_cluster);

    do
    {
        if (dir_count == vfat_info.direntry_per_cluster)
        {
            DEBUG_PRINT("%d -> %d\n", cluster_num, vfat_next_cluster(cluster_num));
            cluster_num = vfat_next_cluster(cluster_num) & 0xfffffff;

            if (cluster_num == FAT32_END_OF_CHAIN)
            {
                DEBUG_PRINT("End of cluster chain!\n");
                break;
            }

            offset = cluster_num * vfat_info.cluster_size;
            dir_count = 0;
        }

        dir_count++;

        if (lseek(vfat_info.fd, offset, SEEK_SET) < offset)
        {
            err(1, "Unable to seek at 0x%zx\n", offset);
        }

        if (read(vfat_info.fd, &dir, sizeof(struct fat32_direntry))
            < sizeof(struct fat32_direntry))
        {
            err(1, "Unable to read entire direntry 0x%zx from disk!\n", offset);
        }

        if (IS_VALID_ENTRY(&dir) && !IS_LFN_ENTRY(dir.attr))
        {
            mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;

            if (dir.attr & VFAT_ATTR_DIR)
            {
                DEBUG_PRINT("Found valid dir: %s\n", dir.nameext);
                mode |= S_IFDIR;
            }
            else
            {
                DEBUG_PRINT("Found valid file: %s\n", dir.nameext);
                mode |= S_IFREG;
            }

            st.st_mode = mode;
            st.st_size = dir.size;
            st.st_ctime = st.st_atime = st.st_mtime = time(NULL);
            st.st_ino = (((uint32_t) dir.cluster_hi) << 16) | dir.cluster_lo;

            DEBUG_PRINT("First cluster at 0x%zx\n", st.st_ino);

            if (callback(callbackdata, dir.nameext, &st, 0) == 1)
            {
                DEBUG_PRINT("Finished reading directory!\n");
                return true;
            }
        }

        offset += sizeof(struct fat32_direntry);
    } while (HAS_MORE_DIRS(&dir));

    return false;
}


// Used by vfat_search_entry()
struct vfat_search_data {
    const char*  name;
    int          found;
    struct stat* st;
};


// You can use this in vfat_resolve as a callback function for vfat_readdir
// This way you can get the struct stat of the subdirectory/file.
int vfat_search_entry(void *data, const char *name, const struct stat *st,
                      off_t offs)
{
    struct vfat_search_data *sd = data;

    if (sd->name != NULL && strcmp(sd->name, name) != 0)
    {
        return 0;
    }

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
    int res = -ENOENT; // Not Found
    char *lp;
    struct vfat_search_data sd;
    uint32_t cluster_number = 0;
    char *path_copy;

    path_copy = calloc(strlen(path) + 1, sizeof(char));

    if (path_copy == NULL)
    {
        DEBUG_PRINT("Could not allocate memory!\n");
        return -ENOMEM;
    }

    strncpy(path_copy, path, strlen(path) + 1);

    lp = strtok(path_copy, "/");
    sd.found = 1;
    sd.st = st;

    DEBUG_PRINT("Looking up %s\n", path);

    while (lp != NULL)
    {
        sd.name = lp;
        sd.found = 0;

        vfat_readdir(cluster_number, vfat_search_entry, &sd);

        if (!sd.found)
        {
            DEBUG_PRINT("%s not found!\n", lp);
            break;
        }

        cluster_number = sd.st->st_ino;

        lp = strtok(NULL, "/");
    }

    if (sd.found)
    {
        DEBUG_PRINT("Successfully found %s\n", path);
        res = 0;
    }

    DEBUG_PRINT("End of directory!\n");

    free(path_copy);

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
int vfat_fuse_getxattr(const char *path, const char* name, char* buf,
                       size_t size)
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
                      fuse_fill_dir_t callback, off_t unused_offs,
                      struct fuse_file_info *unused_fi)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_readdir(path + strlen(DEBUGFS_PATH), callback_data,
                                    callback, unused_offs, unused_fi);
    }

    int res;
    struct stat st;

    res = vfat_resolve(path, &st);

    if (res == 0)
    {
        vfat_readdir(st.st_ino, callback, callback_data);
    }

    return res;
}

int vfat_fuse_read(
                   const char *path, char *buf, size_t size, off_t offs,
                   struct fuse_file_info *unused)
{
    if (strncmp(path, DEBUGFS_PATH, strlen(DEBUGFS_PATH)) == 0) {
        // This is handled by debug virtual filesystem
        return debugfs_fuse_read(path + strlen(DEBUGFS_PATH), buf, size, offs,
                                 unused);
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
