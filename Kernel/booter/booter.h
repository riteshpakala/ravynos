/*
 * Shared declarations for the ravynOS AArch64 UEFI booter.
 */

#ifndef RAVYN_BOOTER_H
#define RAVYN_BOOTER_H

#include "efi.h"
#include <stdarg.h>
#include <stdbool.h>

#define BOOTER_VERSION "0.1"

/* ---- freestanding libc subset (string.c) ---- */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
char  *strchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
unsigned long long strtoull(const char *s, char **end, int base);

/* ---- console (console.c) ---- */
void console_init(EFI_SYSTEM_TABLE *st);
void console_putc(char c);
void console_puts(const char *s);
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...);
void printf(const char *fmt, ...);
void panic(const char *fmt, ...) __attribute__((noreturn));
/* Raw PL011 used after ExitBootServices (0 = none). */
void console_set_pl011(uint64_t base);

/* ---- UEFI helpers (main.c) ---- */
extern EFI_SYSTEM_TABLE  *gST;
extern EFI_BOOT_SERVICES *gBS;
extern EFI_HANDLE         gImageHandle;
void *efi_alloc(size_t bytes);
void *efi_alloc_pages(size_t bytes, EFI_MEMORY_TYPE type);
void *efi_find_config_table(const EFI_GUID *guid);
bool  efi_guid_equal(const EFI_GUID *a, const EFI_GUID *b);
int   efi_read_file(const char *path, void **data, size_t *size);

/* ---- boot plist (plist.c) ---- */
struct boot_config {
	char kernel[256];       /* backslash path on the boot volume */
	char kernel_flags[600];
};
void plist_parse_boot_config(const char *xml, size_t len, struct boot_config *out);

/* ---- flattened device tree (fdt.c) ---- */
struct fdt;
struct fdt *fdt_open(void *blob);
int   fdt_node_by_path(struct fdt *f, const char *path);
int   fdt_node_by_compatible(struct fdt *f, const char *compatible, int start);
int   fdt_node_by_name_prefix(struct fdt *f, const char *prefix, int start);
int   fdt_parent(struct fdt *f, int node);
const char *fdt_node_name(struct fdt *f, int node);
const void *fdt_getprop(struct fdt *f, int node, const char *name, uint32_t *len);
uint32_t fdt_get_u32(struct fdt *f, int node, const char *name, uint32_t dflt);
int   fdt_reg(struct fdt *f, int node, int index, uint64_t *addr, uint64_t *size);
int   fdt_first_subnode(struct fdt *f, int node);
int   fdt_next_subnode(struct fdt *f, int node);
uint64_t fdt_translate(struct fdt *f, int node, uint64_t addr);
uint32_t fdt32(const void *p);

/* ---- Apple device tree (appledt.c) ---- */
struct adt;
struct adt *adt_create(size_t capacity);
int   adt_begin_node(struct adt *t, const char *name);
void  adt_end_node(struct adt *t);
void  adt_prop(struct adt *t, const char *name, const void *value, uint32_t len);
void  adt_prop_str(struct adt *t, const char *name, const char *value);
void  adt_prop_u32(struct adt *t, const char *name, uint32_t value);
void  adt_prop_u64(struct adt *t, const char *name, uint64_t value);
void *adt_finish(struct adt *t, size_t *len);
/* Patch a 16-byte (addr,size) property in place after the tree is placed. */
void *adt_find_prop(void *tree, size_t len, const char *node, const char *prop);

/* ---- Mach-O kernel (macho.c) ---- */
struct kernel_image {
	uint64_t vm_base;       /* lowest segment vmaddr */
	uint64_t vm_end;        /* end of highest segment (page aligned) */
	uint64_t entry;         /* virtual entry point (LC_UNIXTHREAD pc) */
	uint64_t text_base;     /* __TEXT vmaddr */
};
int   macho_inspect(const void *file, size_t size, struct kernel_image *out);
int   macho_load(const void *file, size_t size, const struct kernel_image *img, uint64_t vm_base, uint8_t *phys_base);

/* ---- machine (enter.S / machine.c) ---- */
uint64_t machine_current_el(void);
uint64_t machine_cntfrq(void);
uint64_t machine_mpidr(void);
uint64_t machine_midr(void);
void     machine_clean_dcache(void *start, size_t len);
void     machine_enter_kernel(uint64_t entry_phys, uint64_t boot_args_phys) __attribute__((noreturn));

#endif /* RAVYN_BOOTER_H */
