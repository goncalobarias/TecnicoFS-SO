#include "../../fs/operations.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PATH_FORMAT "/f%ld_%ld"
#define FILE_COUNT 5
#define MAX_PATH_SIZE 32

uint8_t const file_contents[] = "AAA!";

void format_path_(char *dest, size_t max_size, size_t rep, size_t file_idx);
int open_(size_t rep, size_t file_idx, tfs_file_mode_t mode);
int symbolic_link_(size_t rep_target, size_t file_idx_target, size_t rep_name,
                   size_t file_idx_name);
int hard_link_(size_t rep_target, size_t file_idx_target, size_t rep_name,
               size_t file_idx_name);
int unlink_(size_t rep, size_t file_idx);
void assert_contents_ok(size_t rep, size_t file_idx, bool ok_state);
void assert_empty_file(size_t rep, size_t file_idx);
void write_contents(size_t rep, size_t file_idx);
void run_test(size_t rep);

int main() {
    assert(tfs_init(NULL) != -1);

    for (size_t rep = 0; rep < FILE_COUNT; rep++) {
        // create original file
        {
            int fd = open_(rep + 100, rep, TFS_O_CREAT);
            assert(fd != -1);
            assert(tfs_close(fd) != -1);
        }

        assert(hard_link_(rep + 100, rep, rep, rep) != -1);

        // create links
        for (size_t i = 0; i < FILE_COUNT; i++) {
            if (i == rep) {
                continue;
            }

            // instead of linking to the original always, let's create a link to
            // the previously linked name
            if (i == 0) {
                // no links exist yet, link to the original
                assert(symbolic_link_(rep, rep, rep, i) != -1);
            } else {
                // link to the link created in the previous iteration
                assert(symbolic_link_(rep, i - 1, rep, i) != -1);
            }
        }

        run_test(rep);
    }

    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}

void format_path_(char *dest, size_t max_size, size_t rep, size_t file_idx) {
    int ret = snprintf(dest, max_size, PATH_FORMAT, rep, file_idx);
    assert(ret > 0);
    assert(ret <= max_size);
}

int open_(size_t rep, size_t file_idx, tfs_file_mode_t mode) {
    char path[MAX_PATH_SIZE];
    format_path_(path, MAX_PATH_SIZE, rep, file_idx);
    return tfs_open(path, mode);
}

int symbolic_link_(size_t rep_target, size_t file_idx_target, size_t rep_name,
                   size_t file_idx_name) {
    char target[MAX_PATH_SIZE];
    format_path_(target, MAX_PATH_SIZE, rep_target, file_idx_target);

    char name[MAX_PATH_SIZE];
    format_path_(name, MAX_PATH_SIZE, rep_name, file_idx_name);

    return tfs_sym_link(target, name);
}

int hard_link_(size_t rep_target, size_t file_idx_target, size_t rep_name,
               size_t file_idx_name) {
    char target[MAX_PATH_SIZE];
    format_path_(target, MAX_PATH_SIZE, rep_target, file_idx_target);

    char name[MAX_PATH_SIZE];
    format_path_(name, MAX_PATH_SIZE, rep_name, file_idx_name);

    return tfs_link(target, name);
}

int unlink_(size_t rep, size_t file_idx) {
    char path[MAX_PATH_SIZE];
    format_path_(path, MAX_PATH_SIZE, rep, file_idx);
    return tfs_unlink(path);
}

void assert_contents_ok(size_t rep, size_t file_idx, bool ok_state) {
    int f = open_(rep, file_idx, 0);
    if (ok_state) {
        assert(f != -1);
    } else {
        assert(f == -1);
        return;
    }

    uint8_t buffer[sizeof(file_contents)] = {0};
    assert(tfs_read(f, buffer, sizeof(buffer)) == sizeof(buffer));
    assert(memcmp(buffer, file_contents, sizeof(buffer)) == 0);

    assert(tfs_close(f) != -1);
}

void assert_empty_file(size_t rep, size_t file_idx) {
    int f = open_(rep, file_idx, 0);
    assert(f != -1);

    uint8_t buffer[sizeof(file_contents)] = {0};
    assert(tfs_read(f, buffer, sizeof(buffer)) == 0);

    assert(tfs_close(f) != -1);
}

void write_contents(size_t rep, size_t file_idx) {
    int f = open_(rep, file_idx, 0);
    assert(f != -1);

    assert(tfs_write(f, file_contents, sizeof(file_contents)) ==
           sizeof(file_contents));

    assert(tfs_close(f) != -1);
}

void run_test(size_t rep) {
    bool ok_state = true;
    // open_(rep, <number from 0 to FILE_COUNT>, mode) opens one of the
    // alternative names to the same file

    // confirm that all are empty
    for (size_t i = 0; i < FILE_COUNT; i++) {
        assert_empty_file(rep, i);
    }

    // write in one of the links
    write_contents(rep, 3);

    // delete half the links
    for (size_t i = FILE_COUNT / 2; i < FILE_COUNT; i++) {
        assert(unlink_(rep, i) != -1);
        if (rep == i) {
            ok_state = false;
        }
    }

    // confirm that the others see the write
    for (size_t i = 0; i < FILE_COUNT / 2; i++) {
        assert_contents_ok(rep, i, ok_state);
    }

    // finish removing links
    for (size_t i = 0; i < FILE_COUNT / 2; i++) {
        assert(unlink_(rep, i) != -1);
    }
}
