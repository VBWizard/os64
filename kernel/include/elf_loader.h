#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include "elf.h"
#include "task.h"
#include "vfs.h"

typedef struct {
    vfs_file_t *file;
    Elf64_Ehdr ehdr;
    Elf64_Shdr *shdrs;
    char *shstrtab;
    size_t shstrtab_size;
} elf_image_t;

/// @brief Populate task VMAs and entry point from an open ELF file.
int elf_load_task_from_file(task_t *task, vfs_file_t *file);
/// @brief Open an ELF by path and populate task VMAs and entry point.
int elf_load_task_from_path(task_t *task, const char *path);

#endif
