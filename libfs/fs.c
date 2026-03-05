#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

#define ROOT_ENTRY_PADDING 10
#define SIG_LEN 8
#define SIGNATURE "ECS150FS"
#define FAT_EOC 0xFFFF

struct __attribute__((packed)) superblock {
    uint8_t signature[8];
    uint16_t num_blocks;
    uint16_t root_dir_idx;
    uint16_t data_start_idx;
    uint16_t num_data_blocks;
    uint8_t num_fat_blocks;
    uint8_t padding[4079];
};

struct __attribute__((packed)) fat_block {
    uint16_t entries[BLOCK_SIZE / 2];
};

struct __attribute__((packed)) root_entry {
    uint8_t file_name[FS_FILENAME_LEN];
    uint32_t file_size;
    uint16_t index;
    uint8_t padding[ROOT_ENTRY_PADDING];
};

struct __attribute__((packed)) data_block {
    uint8_t entries[BLOCK_SIZE];
};

struct __attribute__((packed)) file_descriptor {
    int file_index;
    size_t file_offset;
};

struct __attribute__((packed)) file_table {
    struct file_descriptor file_entry[FS_OPEN_MAX_COUNT];
};

struct superblock *superblock = NULL;
struct fat_block *fat = NULL;
struct root_entry *root_dir = NULL;
struct file_table *file_table = NULL;
struct data_block *data_block = NULL;

/******************************************************************************************************************
Helper Functions
******************************************************************************************************************/

/**
 * find_file - Finds index of file
 * @filename: File name
 *
 * Searches through the root directory for the first file with a file name equal
 * to @filename.
 *
 * Returns -1 if no FS is mounted, or a file is not found. Otherwise returns the
 * index of the file found in the root directory.
 */
int find_file(const char *filename)
{
    if (root_dir == NULL) {
        return -1;
    }
    for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {
        if (!strcmp((char *)root_dir[i].file_name, filename)) {
            return i;
        }
    }
    return -1;
}

/**
 * is_mounted - Check if file system is mounted
 *
 * Checks if a file system is currently mounted
 *
 * Returns 0 if a file system is not mounted and 1 if it is.
 */
int is_mounted()
{
    if (superblock == NULL || root_dir == NULL || fat == NULL ||
        file_table == NULL) {
        return 0;
    }
    if (block_disk_count() == -1) {
        return 0;
    }
    return 1;
}

/**
 * get_fat_entry() - Gets entry from FAT
 * @index: Index at which entry is gotten from
 *
 * Returns (uint16_t) of value in FAT[index]
 */
uint16_t get_fat_entry(uint16_t index)
{
    uint16_t block_idx = index / (BLOCK_SIZE / 2);
    uint16_t entry_idx = index % (BLOCK_SIZE / 2);
    return fat[block_idx].entries[entry_idx];
}

/**
 * get_fat_next_open() - Get first unallocated block in FAT
 *
 * Returns -1 if fat, superblock not mounted, returns 0 if no free space,
 * returns (uint16_t) of first free.
 */
uint16_t get_fat_next_open()
{
    if (fat == NULL || superblock == NULL) {
        return 0;
    }
    for (int i = 1; i < superblock->num_fat_blocks * BLOCK_SIZE / 2; i++) {
        if (get_fat_entry(i) == 0) {
            return i;
        }
    }
    return 0;
}

/**
 * put_fat_entry() - Put entry into FAT
 * @index: index to be written to
 * @val: value to be written
 *
 * Writes @val to @index of FAT
 *
 */
void put_fat_entry(uint16_t index, uint16_t val)
{
    uint16_t block_idx = index / (BLOCK_SIZE / 2);
    uint16_t entry_idx = index % (BLOCK_SIZE / 2);
    fat[block_idx].entries[entry_idx] = val;
}

/**
 * append_fat_chain() - Inserts index into FAT chain
 * @prev_idx: index which will point to inserted index
 * @new_idx: index which will inherit pointer from previous index
 *
 * Sets fat[new_idx] = fat[prev_idx] then fat[prev_idx] = new_idx
 *
 * Returns 0 if successful, -1 if new_idx out of bounds, new_idx not empty, or
 * prev_idx empty
 */
int append_fat_chain(uint16_t prev_idx, uint16_t new_idx)
{
    if (new_idx >= superblock->num_fat_blocks * BLOCK_SIZE / 2) {
        return -1;
    } else if (get_fat_entry(new_idx) != 0) {
        return -1;
    } else if (get_fat_entry(prev_idx) == 0) {
        return -1;
    }

    put_fat_entry(new_idx, get_fat_entry(prev_idx));
    put_fat_entry(prev_idx, new_idx);
    return 0;
}

/**
 * create_fat_chain() - Initializes a file chain in FAT
 * @start_idx: Index at which file chain is to start
 *
 * Points FAT[start_idx] to FAT_EOC
 * Returns -1 if start_idx already is in file chain
 * Returns 0 if succesful
 */
int create_fat_chain(uint16_t start_idx)
{
    if (get_fat_entry(start_idx) != 0) {
        return -1;
    }
    put_fat_entry(start_idx, FAT_EOC);
    return 0;
}

/******************************************************************************************************************
FS API Functions
******************************************************************************************************************/

int fs_mount(const char *diskname)
{
    int retval = block_disk_open(diskname);
    if (retval != 0) {
        return -1;
    }

    superblock = malloc(sizeof(struct superblock));
    if (superblock == NULL) {
        fs_umount();
        return -1;
    }
    retval = block_read(0, superblock);
    if (retval != 0) {
        fs_umount();
        return -1;
    }

    fat = malloc(sizeof(struct fat_block) * superblock->num_fat_blocks);
    if (fat == NULL) {
        fs_umount();
        return -1;
    }

    root_dir = malloc(sizeof(struct root_entry) * FS_FILE_MAX_COUNT);
    if (root_dir == NULL) {
        fs_umount();
        return -1;
    }

    file_table = malloc(sizeof(struct file_table));
    if (file_table == NULL) {
        fs_umount();
        return -1;
    }

    for (int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
        file_table->file_entry[i].file_index = -1;
        file_table->file_entry[i].file_offset = 0;
    }

    for (int i = 0; i < SIG_LEN; i++) {
        if (SIGNATURE[i] != superblock->signature[i]) {
            fs_umount();
            return -1;
        }
    }

    if (superblock->num_blocks != block_disk_count()) {
        fs_umount();
        return -1;
    }

    for (int i = 1; i < superblock->num_fat_blocks + 1; i++) {
        retval = block_read(i, fat + i - 1);
        if (retval != 0) {
            fs_umount();
            return -1;
        }
    }

    retval = block_read(superblock->root_dir_idx, root_dir);
    if (retval != 0) {
        fs_umount();
        return -1;
    }

    return 0;
}

int fs_umount(void)
{
    uint16_t root_dir_idx = 0;
    uint8_t num_fat = 0;
    int retval = 0;
    if (superblock != NULL) {
        root_dir_idx = superblock->root_dir_idx;
        num_fat = superblock->num_fat_blocks;
        retval = block_write(0, superblock);
        free(superblock);
        superblock = NULL;
    } else {
        retval = -1;
    }

    if (fat != NULL) {
        for (int i = 1; i < num_fat + 1; i++) {
            retval = block_write(i, &fat[i - 1]);
        }
        free(fat);
        fat = NULL;
    } else {
        retval = -1;
    }

    if (root_dir != NULL) {
        retval = block_write(root_dir_idx, root_dir);
        free(root_dir);
        root_dir = NULL;
    } else {
        retval = -1;
    }

    if (file_table != NULL) {
        free(file_table);
        file_table = NULL;
        retval = 0;
    } else {
        retval = -1;
    }

    return retval;
}

int fs_info(void)
{
    if (!is_mounted()) {
        return -1;
    }

    printf("FS Info:\n");
    printf("total_blk_count=%i\n", superblock->num_blocks);
    printf("fat_blk_count=%i\n", superblock->num_fat_blocks);
    printf("rdir_blk=%i\n", superblock->root_dir_idx);
    printf("data_blk=%i\n", superblock->data_start_idx);
    printf("data_blk_count=%i\n", superblock->num_data_blocks);

    int count = 0;
    for (int i = 0; i < superblock->num_fat_blocks; i++) {
        for (int j = 0; j < BLOCK_SIZE / 2; j++) {
            if (fat[i].entries[j] != 0 && count < superblock->num_data_blocks) {
                count++;
            }
        }
    }
    printf("fat_free_ratio=%i/%i\n", superblock->num_data_blocks - count,
           superblock->num_data_blocks);

    count = 0;
    for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {
        if (root_dir[i].file_name[0] == '\0') {
            count++;
        }
    }
    printf("rdir_free_ratio=%i/%i\n", count, FS_FILE_MAX_COUNT);

    return 0;
}

int fs_create(const char *filename)
{
    if (!is_mounted()) {
        return -1;
    }

    int namelen = strnlen(filename, FS_FILENAME_LEN);
    if (namelen <= 0 || namelen > FS_FILENAME_LEN - 1) {
        return -1;
    }
    if (find_file(filename) != -1) {
        return -1;
    }

    int next_free_idx = -1;
    for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {
        if (root_dir[i].file_name[0] == '\0') {
            next_free_idx = i;
            break;
        }
    }
    strcpy((char *)root_dir[next_free_idx].file_name, filename);
    root_dir[next_free_idx].file_size = 0;
    root_dir[next_free_idx].index = FAT_EOC;

    return 0;
}

int fs_delete(const char *filename)
{
    if (!is_mounted()) {
        return -1;
    }

    int namelen = strnlen(filename, FS_FILENAME_LEN);
    if (namelen <= 0 || namelen > FS_FILENAME_LEN - 1) {
        return -1;
    }

    int file_idx = find_file(filename);
    if (file_idx == -1) {
        return -1;
    }

    for (int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
        if (file_table->file_entry[i].file_index == file_idx) {
            return -1;
        }
    }

    root_dir[file_idx].file_name[0] = '\0';
    uint16_t fat_idx = root_dir[file_idx].index;
    while (fat_idx != FAT_EOC) {
        uint16_t prev_fat_idx = fat_idx;
        fat_idx = get_fat_entry(fat_idx);
        put_fat_entry(prev_fat_idx, 0);
    }

    return 0;
}

int fs_ls(void)
{
    if (!is_mounted()) {
        return -1;
    }

    printf("FS Ls:\n");
    for (int i = 0; i < FS_FILE_MAX_COUNT; i++) {
        if (root_dir[i].file_name[0] != '\0') {
            printf("file: %s, size: %d, data_blk: %d\n", root_dir[i].file_name,
                   root_dir[i].file_size, root_dir[i].index);
        }
    }

    return 0;
}

int fs_open(const char *filename)
{
    if (!is_mounted()) {
        return -1;
    }

    int file_idx = find_file(filename);
    if (file_idx == -1) {
        return -1;
    }

    int fd = -1;
    for (int i = 0; i < FS_OPEN_MAX_COUNT; i++) {
        if (file_table->file_entry[i].file_index == -1) {
            fd = i;
            break;
        }
    }
    if (fd == -1) {
        return -1;
    }

    file_table->file_entry[fd].file_index = file_idx;
    file_table->file_entry[fd].file_offset = 0;
    return fd;
}

int fs_close(int fd)
{
    if (!is_mounted()) {
        return -1;
    }
    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT ||
        file_table->file_entry[fd].file_index == -1) {
        return -1;
    }

    file_table->file_entry[fd].file_index = -1;
    file_table->file_entry[fd].file_offset = 0;

    return 0;
}

int fs_stat(int fd)
{
    if (!is_mounted()) {
        return -1;
    }

    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT ||
        file_table->file_entry[fd].file_index == -1) {
        return -1;
    }

    return root_dir[file_table->file_entry[fd].file_index].file_size;
}

int fs_lseek(int fd, size_t offset)
{
    if (!is_mounted()) {
        return -1;
    }
    int max_size = fs_stat(fd);
    if (offset > max_size || offset < 0) {
        return -1;
    }

    file_table->file_entry[fd].file_offset = offset;
    return 0;
}

int fs_write(int fd, void *buf, size_t count)
{
    if (!is_mounted()) {
        return -1;
    }
    int file_idx = file_table->file_entry[fd].file_index;
    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT || file_idx == -1) {
        return -1;
    }
    if (buf == NULL) {
        return -1;
    }

    size_t offset = file_table->file_entry[fd].file_offset;
    uint16_t block_offset = offset / BLOCK_SIZE;
    uint16_t write_byte = offset % BLOCK_SIZE;
    size_t buffer_offset = 0;
    uint8_t flag_fail = 0;

    struct data_block tmp_blk;

    uint16_t curr_idx = root_dir[file_idx].index;
    size_t bytes_written = 0;

    for (int i = 0; i < block_offset; i++) {
        curr_idx = get_fat_entry(curr_idx);
    }

    uint16_t previous_idx = curr_idx;
    while (curr_idx != FAT_EOC && bytes_written < count) {
        int retval =
            block_read(superblock->data_start_idx + curr_idx, &tmp_blk);
        if (retval != 0) {
            flag_fail = 1;
            break;
        }

        size_t iter_bytes_written = 0;
        while (bytes_written + iter_bytes_written < count &&
               write_byte < BLOCK_SIZE) {
            tmp_blk.entries[write_byte] = *((uint8_t *)buf + buffer_offset);
            buffer_offset++;
            write_byte++;
            iter_bytes_written++;
        }
        write_byte = 0;
        retval = block_write(superblock->data_start_idx + curr_idx, &tmp_blk);
        if (retval != 0) {
            flag_fail = 1;
            break;
        }

        bytes_written += iter_bytes_written;
        previous_idx = curr_idx;
        curr_idx = get_fat_entry(curr_idx);
    }

    while (bytes_written < count && !flag_fail) {
        uint16_t next_fat = get_fat_next_open();
        if (next_fat == 0) {
            break;
        }
        for (int i = 0; i < BLOCK_SIZE; i++) {
            tmp_blk.entries[i] = 0;
        }
        size_t iter_bytes_written = 0;
        while (bytes_written + iter_bytes_written < count &&
               write_byte < BLOCK_SIZE) {
            tmp_blk.entries[write_byte] = *((uint8_t *)buf + buffer_offset);
            buffer_offset++;
            write_byte++;
            iter_bytes_written++;
        }
        write_byte = 0;

        int retval =
            block_write(superblock->data_start_idx + next_fat, &tmp_blk);
        if (retval != 0) {
            break;
        }

        if (previous_idx == FAT_EOC) {
            retval = create_fat_chain(next_fat);
            if (retval != 0) {
                break;
            }
            root_dir[file_idx].index = next_fat;
        } else {
            retval = append_fat_chain(previous_idx, next_fat);
        }
        if (retval != 0) {
            break;
        }

        bytes_written += iter_bytes_written;
        previous_idx = next_fat;
    }

    root_dir[file_idx].file_size += bytes_written;
    file_table->file_entry[fd].file_offset += bytes_written;
    return bytes_written;
}

int fs_read(int fd, void *buf, size_t count)
{
    if (!is_mounted()) {
        return -1;
    }

    size_t offset = file_table->file_entry[fd].file_offset;
    uint16_t chain_start_idx = offset / BLOCK_SIZE;
    int file_idx = file_table->file_entry[fd].file_index;

    if (fd < 0 || fd >= FS_OPEN_MAX_COUNT || file_idx == -1) {
        return -1;
    }
    if (buf == NULL) {
        return -1;
    }

    uint16_t curr_idx = root_dir[file_idx].index;
    uint32_t max_offset = root_dir[file_idx].file_size;
    if (offset + count > max_offset) {
        count = max_offset - offset;
    }

    for (int i = 0; i < chain_start_idx; i++) {
        curr_idx = get_fat_entry(curr_idx);
    }

    int buffer_offset = 0;
    size_t start_byte = offset - BLOCK_SIZE * chain_start_idx;
    size_t end_byte = BLOCK_SIZE;

    if (count + start_byte < BLOCK_SIZE) {
        end_byte = count + start_byte;
    }

    struct data_block data_block;
    while (curr_idx != FAT_EOC) {
        int retval =
            block_read(superblock->data_start_idx + curr_idx, &data_block);
        if (retval != 0) {
            break;
        }

        for (int i = start_byte; i < end_byte; i++) {
            uint8_t *ptr = (uint8_t *)buf + buffer_offset;
            *(ptr) = data_block.entries[i];
            buffer_offset++;
            if (buffer_offset >= count) {
                break;
            }
        }
        if (buffer_offset >= count) {
            break;
        }
        start_byte = 0;
        end_byte = (count - buffer_offset) < BLOCK_SIZE
                       ? (count - buffer_offset)
                       : BLOCK_SIZE;

        curr_idx = get_fat_entry(curr_idx);
    }

    file_table->file_entry[fd].file_offset += buffer_offset;

    return buffer_offset;
}
