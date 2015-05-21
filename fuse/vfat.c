// vim: noet:ts=4:sts=4:sw=4:et
#define FUSE_USE_VERSION 26
#define _GNU_SOURCE

#include <assert.h>
#include <ctype.h>
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
#include "lfn.h"
#include "debugfs.h"


char* DEBUGFS_PATH = "/.debug";

#define NAME_LEN 8
#define EXT_LEN 3
#define DIRNAME_LEN NAME_LEN + EXT_LEN + 2

#define ROOT_CLUSTER 2



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


inline void vfat_stat_root(struct stat *st)
{
    st->st_ino = ROOT_CLUSTER;
    st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR;
    st->st_mtime = st->st_ctime = st->st_atime = vfat_info.mount_time;
}

static inline uint32_t offset_of_cluster(uint32_t c)
{
    return (c - ROOT_CLUSTER) * vfat_info.cluster_size
        + vfat_info.cluster_begin_offset;
}

inline void clean_string(char* str, size_t size)
{
    char *pos;

    while ((pos = memchr(str, 0x20, size)) != NULL)
        *pos = 0x00;
}

inline void clean_short_name(struct fat32_direntry *dir)
{
    clean_string(dir->name, NAME_LEN);
    clean_string(dir->ext, EXT_LEN);
}

inline int read_direntry_at(int fd, struct fat32_direntry *dest, off_t offs)
{
    return pread(fd, dest, DIRENTRY_SIZE, offs);
}

inline bool is_lfn_entry_begin(const struct fat32_direntry *dir)
{
    return IS_LFN_ENTRY(dir) && (dir->name[0] & 0x40);
}

void clean_name(char* name, struct fat32_direntry* entry)
{
    char lname[NAME_LEN + 1], lext[EXT_LEN + 1];

    memset(name, 0, DIRNAME_LEN);

    clean_short_name(entry);

    strncpy(lname, entry->name, NAME_LEN);
    strncpy(lext, entry->ext, EXT_LEN);

    lname[NAME_LEN] = 0x00;
    lext[EXT_LEN] = 0x00;

    if (strlen(lext) > 0)
    {
        snprintf(name, DIRNAME_LEN, "%s.%s", lname, lext);
    }
    else
    {
        strncpy(name, lname, NAME_LEN);
    }

    clean_string(name, DIRNAME_LEN);
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

    if (header->sectors_per_fat_small != 0 || header->total_sectors_small != 0)
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

    iconv_utf16 = iconv_open("utf-8", "utf-16");

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
    vfat_info.root_inode.st_ino = 2;

    vfat_info.fat = mmap_file(vfat_info.fd, vfat_info.fat_begin_offset,
                              vfat_info.fat_size);

    if (vfat_info.fat == MAP_FAILED)
    {
        err(1, "Failed to mmap file");
    }
}

uint32_t vfat_next_cluster(uint32_t c)
{
    uint32_t next_cluster_addr;

    if (c > vfat_info.fat_entries)
    {
        return 0xFFFFFFF;
    }

    next_cluster_addr = vfat_info.fat[c];

    DEBUG_PRINT("Cluster chain 0x%x -> 0x%x\n", c, next_cluster_addr);

    return next_cluster_addr & 0xFFFFFFF;
}

int vfat_stat_from_direntry(struct fat32_direntry *dir, struct stat* out)
{
    mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;

    if (IS_DIRECTORY(dir))
    {
        DEBUG_PRINT("Found valid dir: %s\n", dir->name);
        mode |= S_IFDIR;
    }
    else
    {
        DEBUG_PRINT("Found valid file: %s\n", dir->name);
        mode |= S_IFREG;
    }

    out->st_mode = mode;
    out->st_size = dir->size;
    out->st_ino = (((uint32_t) dir->cluster_hi) << 16) | dir->cluster_lo;

    vfat_parse_timestamp(dir, out);

    return 0;
}

int vfat_readdir(uint32_t first_cluster, fuse_fill_dir_t callback,
                 void *callbackdata)
{
    struct stat st; // we can reuse same stat entry over and over again
    uint32_t cluster_num = first_cluster;
    off_t offset;
    struct fat32_direntry dir;
    uint32_t dir_count = 0;
    char real_name[NAME_MAX * sizeof(uint16_t) + 1];
    bool inside_lfn = false;
    uint8_t csum;

    memset(&st, 0, sizeof(st));
    st.st_uid = vfat_info.mount_uid;
    st.st_gid = vfat_info.mount_gid;
    st.st_nlink = 1;

    offset = offset_of_cluster(cluster_num);
    read_direntry_at(vfat_info.fd, &dir, offset);

    DEBUG_PRINT("Reading directory at %x\n", first_cluster);

    while (HAS_MORE_DIRS(&dir))
    {
        if (dir_count == vfat_info.direntry_per_cluster)
        {
            cluster_num = vfat_next_cluster(cluster_num);

            if (cluster_num == FAT32_END_OF_CHAIN)
            {
                DEBUG_PRINT("End of cluster chain!\n");
                break;
            }

            offset = offset_of_cluster(cluster_num);
            dir_count = 0;
        }

        dir_count++;

        if (read_direntry_at(vfat_info.fd, &dir, offset) < 0)
        {
            err(1, "Error while reading directory:");
        }

        offset += DIRENTRY_SIZE;

        DEBUG_PRINT("Tag: 0x%02x\n", (uint8_t) dir.name[0]);

        if (is_valid_direntry(&dir) || IS_LFN_ENTRY(&dir))
        {
            DEBUG_PRINT("Found: 0x%x\n", dir.attr);

            if (is_lfn_entry_begin(&dir))
            {
                DEBUG_PRINT("Starting LFN entry!\n");
                csum = ((struct fat32_direntry_long *) &dir)->csum;
                inside_lfn = true;
            }

            if (IS_LFN_ENTRY(&dir) && inside_lfn)
            {
                int res;
                struct fat32_direntry_long *lng_dir =
                    (struct fat32_direntry_long *) &dir;

                DEBUG_PRINT("Reading LFN direntry!...\n");

                res = read_lfn(lng_dir);

                assertf(lng_dir->csum == csum, "Invalid checksum in LFN\n");

                if (res < 0)
                {
                    err(1, "Unable to read lfn entry:");
                }

                DEBUG_PRINT("Read LFN chunk!\n");

                continue;
            }

            vfat_stat_from_direntry(&dir, &st);

            DEBUG_PRINT("First cluster at 0x%zx\n", st.st_ino);

            int real_csum = calc_csum((uint8_t *) dir.nameext);

            if (get_lfn(real_name) < 0 || real_csum != csum)
            {
                clean_name(real_name, &dir);
            }

            if (callback(callbackdata, (char *) real_name, &st, 0) == 1)
            {
                return 0;
            }
        }
    }

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

    DEBUG_PRINT("Found entry %s\n", name);

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
    int res = 0;
    char *lp;
    struct vfat_search_data sd;
    uint32_t cluster_number = vfat_info.root_inode.st_ino;
    char *path_copy;

    assert(path);
    assert(strlen(path) > 0);
    assert(st);

    path_copy = calloc(strlen(path) + 1, sizeof(char));

    if (path_copy == NULL)
    {
        DEBUG_PRINT("Could not allocate memory!\n");
        return -ENOMEM;
    }

    strncpy(path_copy, path, strlen(path) + 1);

    lp = strtok(path_copy, "/");

    sd.st = st;
    sd.found = 1;

    vfat_stat_root(st);

    DEBUG_PRINT("Looking up %s\n", path);

    while (lp != NULL)
    {
        sd.name = lp;
        sd.found = 0;

        vfat_readdir(cluster_number, vfat_search_entry, &sd);

        if (!sd.found)
        {
            res = -ENOENT;
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
        return debugfs_fuse_getattr(path + strlen(DEBUGFS_PATH), st);
    } else {
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
        res = vfat_readdir(st.st_ino, callback, callback_data);
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

    int res;
    struct stat st;
    off_t offset = 0;
    uint32_t cluster;
    uint32_t real_read;

    res = vfat_resolve(path, &st);

    DEBUG_PRINT("Reading from %s\n", path);

    if (res == 0)
    {
        real_read = MIN((off_t) size, st.st_size - offs);
        real_read = MIN(real_read, vfat_info.cluster_size);

        DEBUG_PRINT("Reading %d starting at 0x%zx\n", real_read, offs);

        cluster = st.st_ino;

        while(offset < offs)
        {
            if (cluster == FAT32_END_OF_CHAIN)
            {
                DEBUG_PRINT("Offset after end of file!\n");
                return 0;
            }

            offset += vfat_info.cluster_size;
            cluster = vfat_next_cluster(cluster);
        }

        offset = offset_of_cluster(cluster) + (offs % vfat_info.cluster_size);

        if (lseek(vfat_info.fd, offset, SEEK_SET) < offset)
        {
            return -errno;
        }

        res = read(vfat_info.fd, buf, real_read);

        DEBUG_PRINT("Read has ended after reading %d bytes\n", res);

        if (res < 0)
        {
            res = -errno;
        }
    }

    return res;
}

////////////// No need to modify anything below this point
int
vfat_opt_args(void *data, const char *arg, int key, struct fuse_args *oargs)
{
    if (key == FUSE_OPT_KEY_NONOPT && !vfat_info.dev) {
        vfat_info.dev = strdup(arg);
        return 0;
    }

    return 1;
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
