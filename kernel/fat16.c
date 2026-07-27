/* fat16.c -- minimal read-only FAT16 filesystem driver
 *
 * Reads the BIOS Parameter Block to find the on-disk layout, searches
 * the (fixed-size, FAT16-style) root directory for a file by its 8.3
 * name, and follows that file's cluster chain through the File
 * Allocation Table to read its full contents.
 */

#include "fat16.h"
#include "ata.h"
#include "serial.h"

struct fat16_bpb {
    unsigned char  jmp[3];
    char           oem[8];
    unsigned short bytes_per_sector;
    unsigned char  sectors_per_cluster;
    unsigned short reserved_sector_count;
    unsigned char  num_fats;
    unsigned short root_entry_count;
    unsigned short total_sectors_16;
    unsigned char  media;
    unsigned short sectors_per_fat;
    unsigned short sectors_per_track;
    unsigned short num_heads;
    unsigned int   hidden_sectors;
    unsigned int   total_sectors_32;
} __attribute__((packed));

struct fat16_dirent {
    char           name[8];
    char           ext[3];
    unsigned char  attr;
    unsigned char  reserved[10];
    unsigned short write_time;
    unsigned short write_date;
    unsigned short first_cluster;
    unsigned int   file_size;
} __attribute__((packed));

#define ATTR_DIRECTORY 0x10
#define ATTR_VOLUME_ID 0x08

static struct fat16_bpb bpb;
static unsigned int fat_start_lba;
static unsigned int root_dir_start_lba;
static unsigned int root_dir_sectors;
static unsigned int data_start_lba;
static int mounted = 0;

static void print_hex(unsigned int n) {
    char buf[9];
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) {
        unsigned int nib = n & 0xF;
        buf[i] = (char)(nib < 10 ? '0' + nib : 'a' + (nib - 10));
        n >>= 4;
    }
    serial_write(buf);
}

int fat16_mount(void) {
    unsigned short sector0[256]; /* unsigned short = 2bytes --> 512byte sector0 = 256 first unsigned shorts*/
    if (!ata_read_sectors(0, 1, sector0)) {
        serial_write("[serial] fat16_mount: failed to read boot sector\n");
        return 0;
    }

    unsigned char *raw = (unsigned char *)sector0;
    unsigned char *dst = (unsigned char *)&bpb;
    for (unsigned int i = 0; i < sizeof(bpb); i++) dst[i] = raw[i];

    if (bpb.bytes_per_sector != 512) {
        serial_write("[serial] fat16_mount: unexpected sector size, refusing to mount\n");
        return 0;
    }

    fat_start_lba = bpb.reserved_sector_count;
    root_dir_start_lba = fat_start_lba + (unsigned int)bpb.num_fats * bpb.sectors_per_fat;
    unsigned int root_dir_bytes = (unsigned int)bpb.root_entry_count * 32;
    root_dir_sectors = (root_dir_bytes + bpb.bytes_per_sector - 1) / bpb.bytes_per_sector;
    data_start_lba = root_dir_start_lba + root_dir_sectors;

    serial_write("[serial] fat16_mount: bytes/sector="); print_hex(bpb.bytes_per_sector);
    serial_write(" sectors/cluster="); print_hex(bpb.sectors_per_cluster);
    serial_write(" num_fats="); print_hex(bpb.num_fats);
    serial_write(" sectors/fat="); print_hex(bpb.sectors_per_fat);
    serial_write("\n[serial] fat16_mount: fat_lba="); print_hex(fat_start_lba);
    serial_write(" root_lba="); print_hex(root_dir_start_lba);
    serial_write(" root_sectors="); print_hex(root_dir_sectors);
    serial_write(" data_lba="); print_hex(data_start_lba);
    serial_write("\n");

    mounted = 1;
    return 1;
}

static unsigned int cluster_to_lba(unsigned short cluster) {
    return data_start_lba + ((unsigned int)cluster - 2) * bpb.sectors_per_cluster; /* cluster 2 is the first usable cluster in FAT16 */
}

/* Converts a normal "NAME.EXT" string into FAT's fixed 8+3, space
 * padded, uppercase, no-dot on-disk format for direct comparison
 * against directory entries. */
static void to_fat_name(const char *input, char out_name[8], char out_ext[3]) {
    for (int i = 0; i < 8; i++) out_name[i] = ' ';
    for (int i = 0; i < 3; i++) out_ext[i] = ' ';

    int i = 0;
    int oi = 0;
    while (input[i] && input[i] != '.' && oi < 8) {
        char c = input[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out_name[oi++] = c;
        i++;
    }
    while (input[i] && input[i] != '.') i++; /* skip rest of name if truncated */
    if (input[i] == '.') {
        i++;
        int ei = 0;
        while (input[i] && ei < 3) {
            char c = input[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out_ext[ei++] = c;
            i++;
        }
    }
}

int fat16_find_file(const char *filename, struct fat16_file *out) {
    if (!mounted) return 0;

    char want_name[8], want_ext[3];
    to_fat_name(filename, want_name, want_ext);

    unsigned short sector_buf[256];
    struct fat16_dirent *entries;

    for (unsigned int s = 0; s < root_dir_sectors; s++) {
        if (!ata_read_sectors(root_dir_start_lba + s, 1, sector_buf)) return 0;
        entries = (struct fat16_dirent *)sector_buf;

        for (int e = 0; e < 16; e++) { /* 512 bytes / 32-byte entries = 16 per sector */
            unsigned char first_byte = (unsigned char)entries[e].name[0];
            if (first_byte == 0x00) return 0; /* end of directory, no more entries at all */
            if (first_byte == 0xE5) continue;  /* deleted entry */
            if (entries[e].attr & ATTR_VOLUME_ID) continue;
            if (entries[e].attr & ATTR_DIRECTORY) continue;

            int match = 1;
            for (int c = 0; c < 8; c++) if (entries[e].name[c] != want_name[c]) match = 0;
            for (int c = 0; c < 3; c++) if (entries[e].ext[c]  != want_ext[c])  match = 0;

            if (match) {
                out->first_cluster = entries[e].first_cluster;
                out->file_size = entries[e].file_size;
                return 1;
            }
        }
    }
    return 0;
}

int fat16_read_file(const struct fat16_file *file, unsigned char *buffer, unsigned int buffer_size) {
    if (!mounted) return 0;

    unsigned short cluster = file->first_cluster;
    unsigned int bytes_read = 0;
    unsigned int cluster_bytes = (unsigned int)bpb.sectors_per_cluster * bpb.bytes_per_sector;

    unsigned short fat_sector_cache[256];
    unsigned int cached_fat_sector = 0xFFFFFFFF;

    while (cluster >= 2 && cluster < 0xFFF8 && bytes_read < file->file_size) {
        unsigned int lba = cluster_to_lba(cluster);
        unsigned int to_read = cluster_bytes;
        if (bytes_read + to_read > buffer_size) to_read = buffer_size - bytes_read;
        if (bytes_read + to_read > file->file_size) to_read = file->file_size - bytes_read;

        unsigned short cluster_buf[256 * 8]; /* supports up to 8 sectors/cluster (4KiB clusters) */
        unsigned char sectors_needed = (unsigned char)bpb.sectors_per_cluster;
        if (!ata_read_sectors(lba, sectors_needed, cluster_buf)) return 0;

        const unsigned char *src = (const unsigned char *)cluster_buf;
        for (unsigned int b = 0; b < to_read; b++) buffer[bytes_read + b] = src[b];
        bytes_read += to_read;

        /* walk the FAT to find the next cluster in the chain */
        unsigned int fat_offset = (unsigned int)cluster * 2; /* 2 bytes per FAT16 entry */
        unsigned int fat_sector = fat_start_lba + (fat_offset / bpb.bytes_per_sector);
        unsigned int fat_index  = (fat_offset % bpb.bytes_per_sector) / 2;

        if (fat_sector != cached_fat_sector) {
            if (!ata_read_sectors(fat_sector, 1, fat_sector_cache)) return 0;
            cached_fat_sector = fat_sector;
        }
        cluster = fat_sector_cache[fat_index];
    }

    return 1;
}

/* Calls callback(name_8_3_formatted, file_size) once per regular file
 * in the root directory. Formats the on-disk 8.3 fields back into a
 * normal "NAME.EXT" (or just "NAME" if no extension) string. */
void fat16_list_root(void (*callback)(const char *name, unsigned int size)) {
    if (!mounted) return;

    unsigned short sector_buf[256];
    struct fat16_dirent *entries;
    char name_out[13]; /* 8 + '.' + 3 + '\0' */

    for (unsigned int s = 0; s < root_dir_sectors; s++) {
        if (!ata_read_sectors(root_dir_start_lba + s, 1, sector_buf)) return;
        entries = (struct fat16_dirent *)sector_buf;

        for (int e = 0; e < 16; e++) {
            unsigned char first_byte = (unsigned char)entries[e].name[0];
            if (first_byte == 0x00) return;
            if (first_byte == 0xE5) continue;
            if (entries[e].attr & ATTR_VOLUME_ID) continue;
            if (entries[e].attr & ATTR_DIRECTORY) continue;

            int ni = 0;
            for (int c = 0; c < 8 && entries[e].name[c] != ' '; c++) name_out[ni++] = entries[e].name[c];
            if (entries[e].ext[0] != ' ') {
                name_out[ni++] = '.';
                for (int c = 0; c < 3 && entries[e].ext[c] != ' '; c++) name_out[ni++] = entries[e].ext[c];
            }
            name_out[ni] = '\0';

            callback(name_out, entries[e].file_size);
        }
    }
}

/* --- write support --- */

#define FAT_ENTRY_FREE 0x0000
#define FAT_ENTRY_END  0xFFFF

static int fat_read_entry(unsigned short cluster, unsigned short *out) {
    unsigned int fat_offset = (unsigned int)cluster * 2;
    unsigned int fat_sector = fat_start_lba + (fat_offset / bpb.bytes_per_sector);
    unsigned int fat_index  = (fat_offset % bpb.bytes_per_sector) / 2;

    unsigned short sector_buf[256];
    if (!ata_read_sectors(fat_sector, 1, sector_buf)) return 0;
    *out = sector_buf[fat_index];
    return 1;
}

/* Writes one FAT entry to *both* on-disk FAT copies -- real FAT
 * volumes keep a backup copy for exactly this write path, and other
 * tools/OSes reading this disk later would only trust FAT copy 1 by
 * default, but keeping copy 2 in sync is the correct/expected thing
 * to do and cheap to get right. */
static int fat_write_entry(unsigned short cluster, unsigned short value) {
    unsigned int fat_offset = (unsigned int)cluster * 2;
    unsigned int fat_sector_in_fat = fat_offset / bpb.bytes_per_sector;
    unsigned int fat_index = (fat_offset % bpb.bytes_per_sector) / 2;

    unsigned short sector_buf[256];

    for (unsigned int copy = 0; copy < bpb.num_fats; copy++) {
        unsigned int lba = fat_start_lba + copy * bpb.sectors_per_fat + fat_sector_in_fat;
        if (!ata_read_sectors(lba, 1, sector_buf)) return 0;
        sector_buf[fat_index] = value;
        if (!ata_write_sectors(lba, 1, sector_buf)) return 0;
    }
    return 1;
}

/* Scans the FAT linearly for a free (0x0000) entry. Fine for a small
 * disk and a demo filesystem; a real implementation would cache the
 * position of the last allocation to avoid rescanning from the start
 * every time. */
static int fat_alloc_cluster(unsigned short *out_cluster) {
    unsigned int total_clusters =
        ((bpb.total_sectors_16 - data_start_lba) / bpb.sectors_per_cluster) + 2;

    for (unsigned short c = 2; c < total_clusters; c++) {
        unsigned short val;
        if (!fat_read_entry(c, &val)) return 0;
        if (val == FAT_ENTRY_FREE) {
            if (!fat_write_entry(c, FAT_ENTRY_END)) return 0; /* claim it immediately */
            *out_cluster = c;
            return 1;
        }
    }
    return 0; /* disk full */
}

static void fat_free_chain(unsigned short start_cluster) {
    unsigned short cluster = start_cluster;
    while (cluster >= 2 && cluster < 0xFFF8) {
        unsigned short next;
        if (!fat_read_entry(cluster, &next)) return;
        fat_write_entry(cluster, FAT_ENTRY_FREE);
        cluster = next;
    }
}

/* Finds an existing entry with this name, OR the first free directory
 * slot if it doesn't exist yet. Returns the (sector, index-within-
 * sector) location either way, and whether it already existed. */
static int find_dirent_slot(const char *filename, unsigned int *out_sector,
                             int *out_index, int *out_existed,
                             struct fat16_dirent *out_existing) {
    char want_name[8], want_ext[3];
    to_fat_name(filename, want_name, want_ext);

    unsigned short sector_buf[256];
    struct fat16_dirent *entries;
    unsigned int free_sector = 0;
    int free_index = -1;

    for (unsigned int s = 0; s < root_dir_sectors; s++) {
        if (!ata_read_sectors(root_dir_start_lba + s, 1, sector_buf)) return 0;
        entries = (struct fat16_dirent *)sector_buf;

        for (int e = 0; e < 16; e++) {
            unsigned char first_byte = (unsigned char)entries[e].name[0];

            if (first_byte == 0x00 || first_byte == 0xE5) {
                if (free_index == -1) { free_sector = root_dir_start_lba + s; free_index = e; }
                if (first_byte == 0x00) goto done_scanning; /* nothing further can match either */
                continue;
            }
            if (entries[e].attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;

            int match = 1;
            for (int c = 0; c < 8; c++) if (entries[e].name[c] != want_name[c]) match = 0;
            for (int c = 0; c < 3; c++) if (entries[e].ext[c]  != want_ext[c])  match = 0;

            if (match) {
                *out_sector = root_dir_start_lba + s;
                *out_index = e;
                *out_existed = 1;
                *out_existing = entries[e];
                return 1;
            }
        }
    }
done_scanning:
    if (free_index == -1) return 0; /* root directory full */
    *out_sector = free_sector;
    *out_index = free_index;
    *out_existed = 0;
    return 1;
}

int fat16_write_file(const char *filename, const unsigned char *data, unsigned int size) {
    if (!mounted) return 0;

    unsigned int dirent_sector;
    int dirent_index, existed;
    struct fat16_dirent existing;

    if (!find_dirent_slot(filename, &dirent_sector, &dirent_index, &existed, &existing)) {
        serial_write("[serial] fat16_write_file: root directory full\n");
        return 0;
    }

    if (existed && existing.first_cluster != 0) {
        fat_free_chain(existing.first_cluster);
    }

    unsigned int cluster_bytes = (unsigned int)bpb.sectors_per_cluster * bpb.bytes_per_sector;
    unsigned int clusters_needed = size == 0 ? 0 : (size + cluster_bytes - 1) / cluster_bytes;

    unsigned short first_cluster = 0;
    unsigned short prev_cluster = 0;
    unsigned int bytes_written = 0;

    for (unsigned int i = 0; i < clusters_needed; i++) {
        unsigned short cluster;
        if (!fat_alloc_cluster(&cluster)) {
            serial_write("[serial] fat16_write_file: disk full\n");
            if (first_cluster) fat_free_chain(first_cluster);
            return 0;
        }
        if (i == 0) first_cluster = cluster;
        else fat_write_entry(prev_cluster, cluster); /* link previous -> this one */
        prev_cluster = cluster;

        unsigned short cluster_buf[256 * 8];
        unsigned char *dst = (unsigned char *)cluster_buf;
        for (unsigned int z = 0; z < cluster_bytes; z++) dst[z] = 0;

        unsigned int to_copy = cluster_bytes;
        if (bytes_written + to_copy > size) to_copy = size - bytes_written;
        for (unsigned int b = 0; b < to_copy; b++) dst[b] = data[bytes_written + b];
        bytes_written += to_copy;

        unsigned int lba = cluster_to_lba(cluster);
        if (!ata_write_sectors(lba, (unsigned char)bpb.sectors_per_cluster, cluster_buf)) return 0;
    }
    if (clusters_needed > 0) fat_write_entry(prev_cluster, FAT_ENTRY_END);

    /* write (or overwrite) the directory entry */
    unsigned short sector_buf[256];
    if (!ata_read_sectors(dirent_sector, 1, sector_buf)) return 0;
    struct fat16_dirent *entries = (struct fat16_dirent *)sector_buf;
    struct fat16_dirent *e = &entries[dirent_index];

    char want_name[8], want_ext[3];
    to_fat_name(filename, want_name, want_ext);
    for (int c = 0; c < 8; c++) e->name[c] = want_name[c];
    for (int c = 0; c < 3; c++) e->ext[c] = want_ext[c];
    e->attr = 0x20; /* archive bit, regular file */
    for (int c = 0; c < 10; c++) e->reserved[c] = 0;
    e->write_time = 0;
    e->write_date = 0;
    e->first_cluster = first_cluster;
    e->file_size = size;

    if (!ata_write_sectors(dirent_sector, 1, sector_buf)) return 0;

    serial_write("[serial] fat16_write_file: wrote '");
    serial_write(filename);
    serial_write("'\n");
    return 1;
}

/* Frees a file's cluster chain and marks its directory entry deleted
 * (first byte 0xE5, the standard FAT convention -- find_dirent_slot's
 * scan already knows to skip these and to treat one as a reusable
 * free slot for a future write). Returns 0 if the file doesn't exist. */
int fat16_delete_file(const char *filename) {
    if (!mounted) return 0;

    unsigned int dirent_sector;
    int dirent_index, existed;
    struct fat16_dirent existing;

    if (!find_dirent_slot(filename, &dirent_sector, &dirent_index, &existed, &existing)) {
        return 0;
    }
    if (!existed) {
        return 0; /* nothing to delete */
    }

    if (existing.first_cluster != 0) {
        fat_free_chain(existing.first_cluster);
    }

    unsigned short sector_buf[256];
    if (!ata_read_sectors(dirent_sector, 1, sector_buf)) return 0;
    struct fat16_dirent *entries = (struct fat16_dirent *)sector_buf;
    entries[dirent_index].name[0] = (char)0xE5;

    if (!ata_write_sectors(dirent_sector, 1, sector_buf)) return 0;

    serial_write("[serial] fat16_delete_file: deleted '");
    serial_write(filename);
    serial_write("'\n");
    return 1;
}
