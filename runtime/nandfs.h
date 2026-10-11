#pragma once
#include "rt.h"
int nand_path_ok(const char *p);
int nand_stat(const char *p, int *is_dir, u32 *size);
int nand_mkdir(const char *p);
int nand_create(const char *p);
int nand_delete(const char *p);
int nand_rename(const char *a, const char *b);
int nand_readdir(const char *p, int idx, char *name, int max);
int nand_read(const char *p, u32 off, u8 *dst, u32 len);
int nand_write(const char *p, u32 off, const u8 *src, u32 len);
int nand_put(const char *p, const u8 *src, u32 len);
void nand_mkdirs(const char *p);
u32 nandfs_serialize(u8 **out);
void nandfs_deserialize(const u8 *b, u32 sz);
void nandfs_load_host(const char *dir);
void nandfs_save_host(void);
extern int g_nand_dirty;
