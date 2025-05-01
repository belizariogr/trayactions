#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

int ensure_dir_exists(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = 0; 
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            struct stat st = {0};
            if (stat(tmp, &st) == -1) {
                if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                    perror("mkdir error");
                    fprintf(stderr, "Failed to create directory: %s\n", tmp);
                    return -1;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                fprintf(stderr, "Error: %s exists but is not a directory\n", tmp);
                return -1;
            }
            *p = '/';
        }
    }

    struct stat st = {0};
    if (stat(tmp, &st) == -1) {
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
            perror("mkdir error");
            fprintf(stderr, "Failed to create directory: %s\n", tmp);
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s exists but is not a directory\n", tmp);
        return -1;
    }
    return 0;
}