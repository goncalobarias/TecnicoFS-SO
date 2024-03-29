#include "../../fs/operations.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

char *str = "AAA!";
char *path = "/f1";
char buffer[40] = {0};

int main() {
    assert(tfs_init(NULL) != -1);

    int f;
    ssize_t r;

    f = tfs_open(path, TFS_O_CREAT);
    assert(f != -1);

    r = tfs_write(f, str, strlen(str));
    assert(r == strlen(str));

    assert(tfs_close(f) != -1);

    f = tfs_open(path, 0);
    assert(f != -1);

    r = tfs_read(f, buffer, sizeof(buffer) - 1);
    assert(r == strlen(str));

    buffer[r] = '\0';
    assert(strcmp(buffer, str) == 0);

    assert(tfs_close(f) != -1);

    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}
