/*
 *      File: state.c
 *      Authors: Gonçalo Sampaio Bárias (ist1103124)
 *               Pedro Perez Vieira (ist1100064)
 *      Description: Persistent FS state.
 */

#include "state.h"
#include "../utils/better-assert.h"
#include "../utils/better-locks.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/**
 * Persistent FS state
 * (in reality, it should be maintained in secondary memory;
 * for simplicity, this project maintains it in primary memory).
 */
static tfs_params fs_params;

// Inode table
static inode_t *inode_table;
pthread_rwlock_t *inode_locks;
static allocation_state_t *freeinode_ts;
static pthread_rwlock_t freeinode_ts_lock;

// Data blocks
static char *fs_data; // # blocks * block size
static pthread_rwlock_t *dir_locks;
static allocation_state_t *free_blocks;
static pthread_mutex_t free_blocks_mutex;

// Volatile FS state
static open_file_entry_t *open_file_table;
static allocation_state_t *free_open_file_entries;
pthread_mutex_t free_open_file_entries_mutex;

// Convenience macros
#define INODE_TABLE_SIZE (fs_params.max_inode_count)
#define DATA_BLOCKS (fs_params.max_block_count)
#define MAX_OPEN_FILES (fs_params.max_open_files_count)
#define BLOCK_SIZE (fs_params.block_size)
#define MAX_DIR_ENTRIES (BLOCK_SIZE / sizeof(dir_entry_t))

static inline bool valid_inumber(int inumber) {
    return inumber >= 0 && inumber < INODE_TABLE_SIZE;
}

static inline bool valid_block_number(int block_number) {
    return block_number >= 0 && block_number < DATA_BLOCKS;
}

static inline bool valid_file_handle(int file_handle) {
    return file_handle >= 0 && file_handle < MAX_OPEN_FILES;
}

static inline bool valid_file_content(int inumber) {
    return valid_inumber(inumber) && freeinode_ts[inumber] == TAKEN;
}

/**
 * Do nothing, while preventing the compiler from performing any optimizations.
 *
 * We need to defeat the optimizer for the insert_delay() function.
 * Under optimization, the empty loop would be completely optimized away.
 * This function tells the compiler that the assembly code being run (which is
 * none) might potentially change *all memory in the process*.
 *
 * This prevents the optimizer from optimizing this code away, because it does
 * not know what it does and it may have side effects.
 *
 * Reference with more information: https://youtu.be/nXaxk27zwlk?t=2775
 *
 * Exercise: try removing this function and look at the assembly generated to
 * compare.
 */
static void touch_all_memory(void) { __asm volatile("" : : : "memory"); }

/**
 * Artifically delay execution (busy loop).
 *
 * Auxiliary function to insert a delay.
 * Used in accesses to persistent FS state as a way of emulating access
 * latencies as if such data structures were really stored in secondary memory.
 */
static void insert_delay(void) {
    for (int i = 0; i < DELAY; i++) {
        touch_all_memory();
    }
}

/**
 * Initialize FS state.
 *
 * Input:
 *   - params: TécnicoFS parameters
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - TFS already initialized.
 *   - malloc failure when allocating TFS structures.
 */
int state_init(tfs_params params) {
    fs_params = params;

    if (inode_table != NULL) {
        return -1; // already initialized
    }

    inode_table = malloc(INODE_TABLE_SIZE * sizeof(inode_t));
    inode_locks = malloc(INODE_TABLE_SIZE * sizeof(pthread_rwlock_t));
    freeinode_ts = malloc(INODE_TABLE_SIZE * sizeof(allocation_state_t));
    fs_data = malloc(DATA_BLOCKS * BLOCK_SIZE);
    dir_locks = malloc(DATA_BLOCKS * sizeof(pthread_rwlock_t));
    free_blocks = malloc(DATA_BLOCKS * sizeof(allocation_state_t));
    open_file_table = malloc(MAX_OPEN_FILES * sizeof(open_file_entry_t));
    free_open_file_entries =
        malloc(MAX_OPEN_FILES * sizeof(allocation_state_t));

    if (!inode_table || !inode_locks || !freeinode_ts || !fs_data ||
        !dir_locks || !free_blocks || !open_file_table ||
        !free_open_file_entries) {
        return -1; // allocation failed
    }

    for (size_t i = 0; i < INODE_TABLE_SIZE; i++) {
        freeinode_ts[i] = FREE;
        rwlock_init(&inode_locks[i]);
    }
    rwlock_init(&freeinode_ts_lock);

    for (size_t i = 0; i < DATA_BLOCKS; i++) {
        free_blocks[i] = FREE;
        rwlock_init(&dir_locks[i]);
    }
    mutex_init(&free_blocks_mutex);

    for (size_t i = 0; i < MAX_OPEN_FILES; i++) {
        free_open_file_entries[i] = FREE;
        mutex_init(&open_file_table[i].mutex);
    }
    mutex_init(&free_open_file_entries_mutex);

    return 0;
}

/**
 * Destroy FS state.
 *
 * Returns 0 if successful, -1 otherwise.
 */
int state_destroy(void) {
    if (inode_table == NULL) {
        return -1; // already destroyed
    }

    for (size_t i = 0; i < INODE_TABLE_SIZE; i++) {
        rwlock_destroy(&inode_locks[i]);
    }
    rwlock_destroy(&freeinode_ts_lock);

    for (size_t i = 0; i < DATA_BLOCKS; i++) {
        rwlock_destroy(&dir_locks[i]);
    }
    mutex_destroy(&free_blocks_mutex);

    for (size_t i = 0; i < MAX_OPEN_FILES; i++) {
        mutex_destroy(&open_file_table[i].mutex);
    }
    mutex_destroy(&free_open_file_entries_mutex);

    free(inode_table);
    free(inode_locks);
    free(freeinode_ts);
    free(fs_data);
    free(dir_locks);
    free(free_blocks);
    free(open_file_table);
    free(free_open_file_entries);

    inode_table = NULL;
    inode_locks = NULL;
    freeinode_ts = NULL;
    fs_data = NULL;
    dir_locks = NULL;
    free_blocks = NULL;
    open_file_table = NULL;
    free_open_file_entries = NULL;

    return 0;
}

size_t state_block_size(void) { return BLOCK_SIZE; }

/**
 * (Try to) Allocate a new inode in the inode table, without initializing its
 * data.
 *
 * Returns the inumber of the newly allocated inode, or -1 in the case of error.
 *
 * Possible errors:
 *   - No free slots in inode table.
 */
static int inode_alloc(void) {
    rwlock_wrlock(&freeinode_ts_lock);
    for (size_t inumber = 0; inumber < INODE_TABLE_SIZE; inumber++) {
        if ((inumber * sizeof(allocation_state_t) % BLOCK_SIZE) == 0) {
            insert_delay(); // simulate storage access delay (to freeinode_ts)
        }

        // Finds first free entry in inode table
        if (freeinode_ts[inumber] == FREE) {
            // Found a free entry, so takes it for the new inode
            freeinode_ts[inumber] = TAKEN;
            rwlock_unlock(&freeinode_ts_lock);

            return (int)inumber;
        }
    }
    rwlock_unlock(&freeinode_ts_lock);

    return -1; // no free inodes
}

/**
 * Create a new inode in the inode table.
 *
 * Allocates and initializes a new inode.
 * Directories will have their data block allocated and initialized, with i_size
 * set to BLOCK_SIZE. Regular files will not have their data block allocated
 * (i_size will be set to 0, i_data_block to -1).
 *
 * Input:
 *   - i_type: the type of the node (file or directory)
 *
 * Returns inumber of the new inode, or -1 in the case of error.
 *
 * Possible errors:
 *   - No free slots in inode table.
 *   - (if creating a directory) No free data blocks.
 */
int inode_create(inode_type i_type) {
    int inumber = inode_alloc();
    if (inumber == -1) {
        return -1; // no free slots in inode table
    }

    inode_t *inode = &inode_table[inumber];
    insert_delay(); // simulate storage access delay (to inode)

    inode->i_node_type = i_type;
    switch (i_type) {
    case T_DIRECTORY: {
        // Initializes directory (filling its block with empty entries, labeled
        // with inumber==-1)
        int b = data_block_alloc();
        if (b == -1) {
            // ensure fields are initialized
            inode->i_size = 0;
            inode->i_data_block = -1;

            // run regular deletion process
            inode_delete(inumber);
            return -1;
        }

        inode_table[inumber].i_size = BLOCK_SIZE;
        inode_table[inumber].i_data_block = b;

        dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(b);
        ALWAYS_ASSERT(dir_entry != NULL,
                      "inode_create: data block freed while in use");

        for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
            dir_entry[i].d_inumber = -1;
        }
    } break;
    case T_FILE:
    case T_SYM_LINK:
        // In case of a new file or symbolic link, simply sets its size to 0
        inode_table[inumber].i_size = 0;
        inode_table[inumber].i_data_block = -1;
        break;
    default:
        PANIC("inode_create: unknown file type");
    }
    inode_table[inumber].i_hard_links = 1;

    return inumber;
}

/**
 * Delete an inode.
 *
 * Input:
 *   - inumber: inode's number
 */
void inode_delete(int inumber) {
    // simulate storage access delay (to inode and freeinode_ts)
    insert_delay();
    insert_delay();

    ALWAYS_ASSERT(valid_inumber(inumber), "inode_delete: invalid inumber");

    rwlock_wrlock(&freeinode_ts_lock);
    ALWAYS_ASSERT(freeinode_ts[inumber] == TAKEN,
                  "inode_delete: inode already freed");

    rwlock_wrlock(&inode_locks[inumber]);
    if (inode_table[inumber].i_size > 0) {
        data_block_free(inode_table[inumber].i_data_block);
    }
    inode_table[inumber].i_size = 0;
    inode_table[inumber].i_data_block = -1;
    inode_table[inumber].i_hard_links = 1;

    freeinode_ts[inumber] = FREE;
    rwlock_unlock(&inode_locks[inumber]);
    rwlock_unlock(&freeinode_ts_lock);
}

/**
 * Obtain a pointer to an inode from its inumber.
 *
 * Input:
 *   - inumber: inode's number
 *
 * Returns pointer to inode.
 */
inode_t *inode_get(int inumber) {
    ALWAYS_ASSERT(valid_inumber(inumber), "inode_get: invalid inumber");

    insert_delay(); // simulate storage access delay to inode
    return &inode_table[inumber];
}

size_t inode_table_size(void) { return INODE_TABLE_SIZE; }

/**
 * Store the inumber for a sub file in a directory.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *   - sub_inumber: inumber of the sub inode
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - sub_name is not a valid file name (length 0 or > MAX_FILE_NAME - 1).
 *   - Directory is already full of entries.
 */
int add_dir_entry(inode_t *inode, char const *sub_name, int sub_inumber) {
    ALWAYS_ASSERT(inode != NULL, "add_dir_entry: inode must be non-NULL");
    ALWAYS_ASSERT(sub_name != NULL, "add_dir_entry: sub_name must be non-NULL");

    if (strlen(sub_name) == 0 || strlen(sub_name) > MAX_FILE_NAME - 1 ||
        !valid_inumber(sub_inumber)) {
        return -1; // invalid sub_name or invalid sub_inumber
    }

    insert_delay(); // simulate storage access delay to inode with inumber
    if (inode->i_node_type != T_DIRECTORY) {
        return -1; // not a directory
    }

    // Locates the block containing the entries of the directory
    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    ALWAYS_ASSERT(dir_entry != NULL,
                  "add_dir_entry: directory must have a data block");

    rwlock_wrlock(&dir_locks[inode->i_data_block]);
    // Makes sure another entry with the same name doesn't exist
    for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
        if ((dir_entry[i].d_inumber != -1) &&
            (strcmp(dir_entry[i].d_name, sub_name) == 0)) {
            rwlock_unlock(&dir_locks[inode->i_data_block]);
            return -1;
        }
    }

    // Finds and fills the first empty entry
    for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (dir_entry[i].d_inumber == -1) {
            dir_entry[i].d_inumber = sub_inumber;
            strncpy(dir_entry[i].d_name, sub_name, MAX_FILE_NAME - 1);
            dir_entry[i].d_name[MAX_FILE_NAME - 1] = '\0';
            rwlock_unlock(&dir_locks[inode->i_data_block]);
            return 0;
        }
    }
    rwlock_unlock(&dir_locks[inode->i_data_block]);

    return -1; // no space for entry
}

/**
 * Clear the directory entry associated with a sub file.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *
 * Returns 0 if successful, -1 otherwise.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - Directory does not contain an entry for sub_name.
 */
int clear_dir_entry(inode_t *inode, char const *sub_name) {
    ALWAYS_ASSERT(inode != NULL, "clear_dir_entry: inode must be non-NULL");
    ALWAYS_ASSERT(sub_name != NULL,
                  "clear_dir_entry: sub_name must be non-NULL");

    if (strlen(sub_name) == 0 || strlen(sub_name) > MAX_FILE_NAME - 1) {
        return -1; // invalid sub_name
    }

    insert_delay();
    if (inode->i_node_type != T_DIRECTORY) {
        return -1; // not a directory
    }

    // Locates the block containing the entries of the directory
    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    ALWAYS_ASSERT(dir_entry != NULL,
                  "clear_dir_entry: directory must have a data block");

    rwlock_wrlock(&dir_locks[inode->i_data_block]);
    for (size_t i = 0; i < MAX_DIR_ENTRIES; i++) {
        if (!strcmp(dir_entry[i].d_name, sub_name)) {
            dir_entry[i].d_inumber = -1;
            memset(dir_entry[i].d_name, 0, MAX_FILE_NAME);
            rwlock_unlock(&dir_locks[inode->i_data_block]);
            return 0;
        }
    }
    rwlock_unlock(&dir_locks[inode->i_data_block]);

    return -1; // sub_name not found
}

/**
 * Obtain the inumber for a sub file inside a directory.
 *
 * Input:
 *   - inode: directory inode
 *   - sub_name: sub file name
 *
 * Returns inumber linked to the target name, -1 if errors occur.
 *
 * Possible errors:
 *   - inode is not a directory inode.
 *   - Directory does not contain a file named sub_name.
 */
int find_in_dir(inode_t const *inode, char const *sub_name) {
    ALWAYS_ASSERT(inode != NULL, "find_in_dir: inode must be non-NULL");
    ALWAYS_ASSERT(sub_name != NULL, "find_in_dir: sub_name must be non-NULL");

    if (strlen(sub_name) == 0 || strlen(sub_name) > MAX_FILE_NAME - 1) {
        return -1; // invalid sub_name
    }

    insert_delay(); // simulate storage access delay to inode with inumber
    if (inode->i_node_type != T_DIRECTORY) {
        return -1; // not a directory
    }

    // Locates the block containing the entries of the directory
    dir_entry_t *dir_entry = (dir_entry_t *)data_block_get(inode->i_data_block);
    ALWAYS_ASSERT(dir_entry != NULL,
                  "find_in_dir: directory inode must have a data block");

    rwlock_rdlock(&dir_locks[inode->i_data_block]);
    // Iterates over the directory entries looking for one that has the target
    // name
    for (int i = 0; i < MAX_DIR_ENTRIES; i++) {
        if ((dir_entry[i].d_inumber != -1) &&
            (strncmp(dir_entry[i].d_name, sub_name, MAX_FILE_NAME) == 0)) {
            int sub_inumber = dir_entry[i].d_inumber;
            rwlock_unlock(&dir_locks[inode->i_data_block]);
            return sub_inumber;
        }
    }
    rwlock_unlock(&dir_locks[inode->i_data_block]);

    return -1; // entry not found
}

/**
 * Allocate a new data block.
 *
 * Returns block number/index if successful, -1 otherwise.
 *
 * Possible errors:
 *   - No free data blocks.
 */
int data_block_alloc(void) {
    mutex_lock(&free_blocks_mutex);
    for (size_t i = 0; i < DATA_BLOCKS; i++) {
        if (i * sizeof(allocation_state_t) % BLOCK_SIZE == 0) {
            insert_delay(); // simulate storage access delay to free_blocks
        }

        if (free_blocks[i] == FREE) {
            free_blocks[i] = TAKEN;
            mutex_unlock(&free_blocks_mutex);

            return (int)i;
        }
    }
    mutex_unlock(&free_blocks_mutex);

    return -1; // no free data blocks
}

/**
 * Free a data block.
 *
 * Input:
 *   - block_number: the block number/index
 */
void data_block_free(int block_number) {
    ALWAYS_ASSERT(valid_block_number(block_number),
                  "data_block_free: invalid block number");

    insert_delay(); // simulate storage access delay to free_blocks

    mutex_lock(&free_blocks_mutex);
    free_blocks[block_number] = FREE;
    mutex_unlock(&free_blocks_mutex);
}

/**
 * Obtain a pointer to the contents of a given block.
 *
 * Input:
 *   - block_number: the block number/index
 *
 * Returns a pointer to the first byte of the block.
 */
void *data_block_get(int block_number) {
    ALWAYS_ASSERT(valid_block_number(block_number),
                  "data_block_get: invalid block number");

    insert_delay(); // simulate storage access delay to block

    return &fs_data[(size_t)block_number * BLOCK_SIZE];
}

/**
 * Add a new entry to the open file table.
 *
 * Input:
 *   - inumber: inode number of the file to open
 *   - offset: initial offset
 *
 * Returns file handle if successful, -1 otherwise.
 *
 * Possible errors:
 *   - No space in open file table for a new open file.
 */
int add_to_open_file_table(int inumber, size_t offset) {
    if (!valid_inumber(inumber)) {
        return -1;
    }

    // We have to recheck this because the file could have been deleted
    if (freeinode_ts[inumber] != TAKEN) {
        return -1;
    }
    mutex_lock(&free_open_file_entries_mutex);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (free_open_file_entries[i] == FREE) {
            free_open_file_entries[i] = TAKEN;

            mutex_lock(&open_file_table[i].mutex);
            open_file_table[i].of_inumber = inumber;
            open_file_table[i].of_offset = offset;
            mutex_unlock(&open_file_table[i].mutex);

            mutex_unlock(&free_open_file_entries_mutex);
            return i;
        }
    }
    mutex_unlock(&free_open_file_entries_mutex);

    return -1;
}

/**
 * Free an entry from the open file table.
 *
 * Input:
 *   - fhandle: file handle to free/close
 *
 * Returns 0 if succesful, -1 otherwise.
 */
int remove_from_open_file_table(int fhandle) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1; // invalid fd
    }

    mutex_lock(&free_open_file_entries_mutex);
    mutex_lock(&file->mutex);
    ALWAYS_ASSERT(
        valid_file_content(file->of_inumber),
        "remove_from_open_file_table: file content deleted before closing");

    ALWAYS_ASSERT(valid_file_handle(fhandle),
                  "remove_from_open_file_table: file handle must be valid");

    ALWAYS_ASSERT(free_open_file_entries[fhandle] == TAKEN,
                  "remove_from_open_file_table: file handle must be taken");

    free_open_file_entries[fhandle] = FREE;

    // Deletes unlinked files on the last close (locks the inode because it
    // needs to read the hard link counter)
    rwlock_rdlock(&inode_locks[file->of_inumber]);
    inode_t *file_inode = inode_get(file->of_inumber);
    if (file_inode == NULL) {
        rwlock_unlock(&inode_locks[file->of_inumber]);
        mutex_unlock(&file->mutex);
        mutex_unlock(&free_open_file_entries_mutex);
        return -1;
    }
    if (file_inode->i_hard_links == 0 && is_file_open(file->of_inumber) == 0) {
        rwlock_unlock(&inode_locks[file->of_inumber]);
        inode_delete(file->of_inumber);
    } else {
        rwlock_unlock(&inode_locks[file->of_inumber]);
    }
    mutex_unlock(&file->mutex);
    mutex_unlock(&free_open_file_entries_mutex);

    return 0;
}

/**
 * Writes to an open file handle.
 *
 * Inputs:
 *  - file handle to write to
 *  - buffer to write
 *  - number of bytes to be written
 *
 * Returns the number of bytes written, or -1 if unsuccessful.
 */
ssize_t write_to_open_file(int fhandle, void const *buffer, size_t to_write) {
    if (buffer == NULL) {
        return -1;
    }

    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }
    mutex_lock(&file->mutex);

    // From the open file table entry, we get the inode
    rwlock_wrlock(&inode_locks[file->of_inumber]);
    inode_t *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_write: inode of open file deleted");

    // Just to make sure that the offset isn't out of bounds
    if (file->of_offset > inode->i_size) {
        file->of_offset = inode->i_size;
    }

    // Determine how many bytes to write
    size_t block_size = state_block_size();
    if (to_write + file->of_offset > block_size) {
        to_write = block_size - file->of_offset;
    }

    if (to_write > 0) {
        if (inode->i_size == 0) {
            // If empty file, allocate new block
            int bnum = data_block_alloc();
            if (bnum == -1) {
                rwlock_unlock(&inode_locks[file->of_inumber]);
                mutex_unlock(&file->mutex);

                return -1; // no space
            }
            inode->i_data_block = bnum;
        }

        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_write: data block deleted mid-write");

        // Perform the actual write
        memcpy(block + file->of_offset, buffer, to_write);

        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
    }
    rwlock_unlock(&inode_locks[file->of_inumber]);
    mutex_unlock(&file->mutex);

    return (ssize_t)to_write;
}

/**
 * Reads from an open file handle.
 *
 * Inputs:
 *  - file handle to read from
 *  - buffer to read to
 *  - number of bytes to read
 *
 * Returns the number of bytes read, or -1 if unsuccessful.
 */
ssize_t read_from_open_file(int fhandle, void *buffer, size_t len) {
    if (buffer == NULL) {
        return -1;
    }

    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }
    mutex_lock(&file->mutex);

    // From the open file table entry, we get the inode
    rwlock_rdlock(&inode_locks[file->of_inumber]);
    inode_t const *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_read: inode of open file deleted");

    // Just to make sure that write_to_open_file doesn't make the offset out of
    // bounds
    if (file->of_offset > inode->i_size) {
        file->of_offset = inode->i_size;
    }

    // Determine how many bytes to read
    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    if (to_read > 0) {
        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_read: data block deleted mid-read");

        // Perform the actual read
        memcpy(buffer, block + file->of_offset, to_read);
        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_read;
    }
    rwlock_unlock(&inode_locks[file->of_inumber]);
    mutex_unlock(&file->mutex);

    return (ssize_t)to_read;
}

/**
 * Obtain pointer to a given entry in the open file table.
 *
 * Input:
 *   - fhandle: file handle
 *
 * Returns pointer to the entry, or NULL if the fhandle is invalid/closed/never
 * opened.
 */
open_file_entry_t *get_open_file_entry(int fhandle) {
    if (!valid_file_handle(fhandle) ||
        free_open_file_entries[fhandle] != TAKEN) {
        return NULL;
    }

    return &open_file_table[fhandle];
}

/**
 * Detects if a file associated with the given inumber is opened or not.
 *
 * Input:
 *   - inumber: inumber of a potentially open file
 *
 * Returns 1 if the file associated with the given inumber is opened, 0
 * otherwise.
 */
bool is_file_open(int inumber) {
    if (!valid_inumber(inumber)) {
        return 0;
    }

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (free_open_file_entries[i] == TAKEN &&
            open_file_table[i].of_inumber == inumber) {
            return 1;
        }
    }

    return 0;
}
