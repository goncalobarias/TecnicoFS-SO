#include "operations.h"
#include "betterassert.h"
#include "config.h"
#include "state.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

tfs_params tfs_default_params() {
    tfs_params params = {
        .max_inode_count = 64,
        .max_block_count = 1024,
        .max_open_files_count = 16,
        .block_size = 1024,
    };
    return params;
}

int tfs_init(tfs_params const *params_ptr) {
    tfs_params params;
    if (params_ptr != NULL) {
        params = *params_ptr;
    } else {
        params = tfs_default_params();
    }

    if (state_init(params) != 0) {
        return -1;
    }

    // create root inode
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }

    return 0;
}

int tfs_destroy() {
    if (state_destroy() != 0) {
        return -1;
    }
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}

/**
 * Looks for a file.
 *
 * Note: as a simplification, only a plain directory space (root directory only)
 * is supported.
 *
 * Input:
 *   - name: absolute path name
 *   - root_inode: the root directory inode
 * Returns the inumber of the file, -1 if unsuccessful.
 */
static int tfs_lookup(char const *name, inode_t const *root_inode) {
    if (root_inode == NULL || root_inode->i_data_block != 0) {
        return -1; // root_inode is not the actual root inode
    }

    // Checks if the path name is valid
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;

    return find_in_dir(root_inode, name);
}

int tfs_open(char const *name, tfs_file_mode_t mode) {
    // Checks if the path name is valid
    if (!valid_pathname(name)) {
        return -1;
    }

    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_open: root dir inode must exist");
    int inum = tfs_lookup(name, root_dir_inode);
    size_t offset = 0;

    if (inum >= 0) {
        // The file already exists
        inode_t *inode = inode_get(inum);
        ALWAYS_ASSERT(inode != NULL,
                      "tfs_open: directory files must have an inode");

        if (inode->i_node_type == T_SYM_LINK) {
            int sym_link_handle = add_to_open_file_table(inum, offset);
            char buffer[state_block_size()];
            memset(buffer, 0, sizeof(buffer));
            tfs_read(sym_link_handle, buffer, state_block_size());
            return tfs_open(buffer, mode);
        }

        // Truncate (if requested)
        if (mode & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                data_block_free(inode->i_data_block);
                inode->i_size = 0;
            }
        }
        // Determine initial offset
        if (mode & TFS_O_APPEND) {
            offset = inode->i_size;
        }
    } else if (mode & TFS_O_CREAT) {
        // The file does not exist; the mode specified that it should be created
        // Create inode
        inum = inode_create(T_FILE);
        if (inum == -1) {
            return -1; // no space in inode table
        }

        // Add entry in the root directory
        if (add_dir_entry(root_dir_inode, name + 1, inum) == -1) {
            inode_delete(inum);
            return -1; // no space in directory
        }
    } else {
        return -1;
    }

    // Finally, add entry to the open file table and return the corresponding
    // handle
    return add_to_open_file_table(inum, offset);

    // Note: for simplification, if file was created with TFS_O_CREAT and there
    // is an error adding an entry to the open file table, the file is not
    // opened but it remains created
}

int tfs_sym_link(char const *target, char const *link) {
    // Checks if the path names are valid
    if (!valid_pathname(target) || !valid_pathname(link)) {
        return -1;
    }

    // TODO: fix this to work for subdirectories
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_sym_link: root dir inode must exist");

    int link_handle = tfs_open(link, TFS_O_CREAT);
    if (link_handle == -1) {
        tfs_close(link_handle); // we ignore the value since we return -1
        return -1;
    }
    tfs_write(link_handle, target, strlen(target));
    if (tfs_close(link_handle) == -1) {
        return -1;
    }

    int link_inum = tfs_lookup(link, root_dir_inode);
    if (link_inum == -1) {
        return -1;
    }
    inode_t *link_inode = inode_get(link_inum);
    if (link_inode == NULL) {
        return -1;
    }
    link_inode->i_node_type = T_SYM_LINK;

    add_dir_entry(root_dir_inode, link + 1, link_inum);

    return 0;
}

int tfs_link(char const *target, char const *link) {
    // Checks if the path names are valid
    if (!valid_pathname(target) || !valid_pathname(link)) {
        return -1;
    }

    // TODO: fix this to work for subdirectories
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_link: root dir inode must exist");

    int target_inum = tfs_lookup(target, root_dir_inode);
    if (target_inum == -1) {
        return -1;
    }

    add_dir_entry(root_dir_inode, link + 1, target_inum);

    inode_t *target_inode = inode_get(target_inum);
    if (target_inode == NULL || target_inode->i_node_type == T_SYM_LINK) {
        clear_dir_entry(root_dir_inode, link + 1);
        return -1;
    }
    target_inode->i_hard_links++;

    return 0;
}

int tfs_close(int fhandle) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1; // invalid fd
    }

    remove_from_open_file_table(fhandle);

    return 0;
}

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    // From the open file table entry, we get the inode
    inode_t *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_write: inode of open file deleted");

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

    return (ssize_t)to_write;
}

ssize_t tfs_read(int fhandle, void *buffer, size_t len) {
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        return -1;
    }

    // From the open file table entry, we get the inode
    inode_t const *inode = inode_get(file->of_inumber);
    ALWAYS_ASSERT(inode != NULL, "tfs_read: inode of open file deleted");

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

    return (ssize_t)to_read;
}

int tfs_unlink(char const *target) {
    // Checks if the path names are valid
    if (!valid_pathname(target)) {
        return -1;
    }

    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_link: root dir inode must exist");
    int target_inum = tfs_lookup(target, root_dir_inode);
    if (target_inum == -1) {
        return -1;
    }
    inode_t *target_inode = inode_get(target_inum);
    if (target_inode == NULL || target_inode->i_node_type != T_FILE) {
        return -1;
    }

    target_inode->i_hard_links--;
    if (target_inode->i_hard_links == 0) {
        // TODO: fix this to work for subdirectories
        if (clear_dir_entry(root_dir_inode, target + 1) == -1) {
            return -1;
        }
        inode_delete(target_inum);
    }

    return 0;
}

int tfs_copy_from_external_fs(char const *source_path, char const *dest_path) {
    // Checks if the path name is valid
    if (!valid_pathname(dest_path)) {
        return -1;
    }

    // Opens the file from the external fs
    FILE *source_file = fopen(source_path, "r");
    if (source_file == NULL) {
        return -1;
    }

    // TODO: see if we should copy the BLOCK_SIZE even when the external file is
    // bigger
    // Checks if the file fits in one block
    fseek(source_file, 0L, SEEK_END);
    long int to_read = ftell(source_file);
    if (to_read > state_block_size()) {
        fclose(source_file); // since we return -1, we can ignore the result
        return -1;
    }
    rewind(source_file);

    // Opens the destination file in the FS
    int dest_file = tfs_open(dest_path, TFS_O_CREAT | TFS_O_TRUNC);
    if (dest_file == -1) {
        fclose(source_file); // since we return -1, we can ignore the result
        return -1;
    }

    // Buffers the file data
    char buffer[to_read];
    size_t read = fread(buffer, sizeof(char), (size_t)to_read, source_file);
    if (read < to_read) {
        // we are returning -1 so the return values are ignored
        fclose(source_file);
        tfs_close(dest_file);
        return -1;
    }

    // Writes the data into the file in TecnicoFS
    ssize_t written = tfs_write(dest_file, buffer, (size_t)to_read);

    // Closes the files
    if (fclose(source_file) != 0) {
        tfs_close(dest_file); // since we return -1, we can ignore the result
        return -1;
    }
    if (tfs_close(dest_file) == -1) {
        return -1;
    }
    if (written == -1) {
        return -1;
    }

    return 0;
}
