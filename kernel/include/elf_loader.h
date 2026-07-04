#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "elf.h"
#include "task.h"
#include "vfs.h"

// Cap on DT_NEEDED entries per image — generous for a hobby OS with no
// transitive dependency explosion; raise if a real test case needs more.
#define ELF_MAX_NEEDED 16

typedef struct {
    vfs_file_t *file;
    Elf64_Ehdr ehdr;
    Elf64_Shdr *shdrs;
    char *shstrtab;
    size_t shstrtab_size;

    // Dynamic linking (populated only when a PT_DYNAMIC segment is present).
    bool is_dynamic;
    Elf64_Dyn *dynamic;
    size_t dynamic_count;

    Elf64_Sym *symtab;      // DT_SYMTAB; count comes from DT_HASH's nchain
    size_t symtab_count;
    char *strtab;           // DT_STRTAB, DT_STRSZ bytes
    size_t strtab_size;

    Elf64_Rela *rela;       // DT_RELA / DT_RELASZ    (.rela.dyn)
    size_t rela_count;
    Elf64_Rela *jmprel;     // DT_JMPREL / DT_PLTRELSZ (.rela.plt)
    size_t jmprel_count;

    char *needed[ELF_MAX_NEEDED];  // DT_NEEDED names, pointing into strtab
    size_t needed_count;
} elf_image_t;

// One PT_LOAD segment's page-aligned placement within a dynamically-linked
// image's own vaddr space (vaddr 0 == page 0). vaddr_off/pages describe
// which global page indices this segment covers — used both for sizing a
// shared_object_t's page cache and for the VMAs a task maps it through.
typedef struct {
    uintptr_t vaddr_off;   // page-aligned offset from the image's own base
    size_t pages;
    int prot;              // PROT_READ / PROT_WRITE / PROT_EXEC
} elf_segment_range_t;

// Generous — real segment counts for a hobby OS's binaries are 2-4 (RX, RW,
// occasionally RO-.rodata and a separate BSS-only tail).
#define ELF_MAX_SEGMENTS 16

/// @brief Seek and read a fixed-size block from a file. Exposed (not just
/// used internally by elf_load_from_file) so shared_object.c can read
/// segment/dynamic-section data through the same VFS path.
bool elf_read_at(vfs_file_t *file, uint64_t offset, void *buffer, size_t size);

/// @brief Free an elf_image_t and every table it owns (safe on a partially
/// populated one). Does NOT close image->file — that's the caller's.
void elf_image_free(elf_image_t *image);

/// @brief Parse an ELF64 file's header/program headers/section headers/
/// dynamic section into a freshly allocated elf_image_t, without touching
/// any task or creating VMAs. *out_phdrs is a kmalloc'd copy of the raw
/// PT_LOAD table (caller owns it, must kfree) — used by callers that manage
/// their own physical pages instead of task VMAs (e.g. shared_object.c).
int elf_parse_image(vfs_file_t *file, elf_image_t **out_image,
                     Elf64_Phdr **out_phdrs, Elf64_Half *out_phnum);

/// @brief Compute page-aligned segment ranges and total page span for every
/// PT_LOAD segment, with no I/O — see elf_loader.c for how shared_object.c
/// uses this to size a lazily-populated per-page physical cache instead of
/// loading the whole image up front.
int elf_compute_segment_ranges(const Elf64_Phdr *phdrs, Elf64_Half phnum,
                                size_t *out_total_pages,
                                elf_segment_range_t *out_segs, size_t max_segs, size_t *out_seg_count);

/// @brief Read one page's worth of file content (page_idx is a global page
/// index into the image's own vaddr space) into an already-zeroed `dest`
/// buffer. Called lazily, from the page-fault path, the first time any task
/// touches a given page of a dynamically-linked image.
bool elf_read_page(vfs_file_t *file, const Elf64_Phdr *phdrs, Elf64_Half phnum,
                    size_t page_idx, void *dest);

/// @brief Populate task VMAs and entry point from an open ELF file.
int elf_load_from_file(task_t *task, vfs_file_t *file);
/// @brief Open an ELF by path and populate task VMAs and entry point.
int elf_load_from_path(task_t *task, const char *path);
/// @brief Peek at whether the ELF at `path` has a PT_DYNAMIC segment,
/// without loading it. See elf_loader.c for why task_create needs to know
/// this before choosing a loading strategy.
bool elf_is_dynamic(const char *path);

#endif
