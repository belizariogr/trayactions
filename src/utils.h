#ifndef UTILS_H
#define UTILS_H

/**
 * Create directories if they don't exist.
 * Returns 0 on success, -1 on error.
 */
int ensure_dir_exists(const char *path);

#endif // UTILS_H