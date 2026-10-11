/* hostio.h - stateless read-only access to the user's game files (native: files on disk,
 * browser: imported JS functions that read from the files the user selected/served). */
#pragma once
#include <stdint.h>
int64_t host_file_size(const char *path);                                 /* -1 if missing */
int host_file_read(const char *path, uint64_t off, void *dst, uint32_t len); /* bytes read or -1 */
void *host_file_load(const char *path, uint32_t *len);                    /* malloc'd whole file or NULL */
