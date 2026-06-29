#include "elf_loader.h"

#include "CONFIG.h"
#include "memory/kmalloc.h"
#include "memory/mmap.h"
#include "memory/vma.h"
#include "paging.h"

/// @brief Align value down to the nearest page boundary.
static inline uint64_t align_down(uint64_t value)
{
    return value & ~(PAGE_SIZE - 1);
}

/// @brief Align value up to the nearest page boundary.
static inline uint64_t align_up(uint64_t value)
{
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/// @brief Resolve file operations for a VFS file handle.
static vfs_file_operations_t* elf_get_fops(vfs_file_t *file)
{
    if (file == NULL) {
        return NULL;
    }

    if (file->fops != NULL) {
        return file->fops;
    }

    if (file->owner != NULL) {
        return ((vfs_filesystem_t *)file->owner)->fops;
    }

    return NULL;
}

/// @brief Seek and read a fixed-size block from a file.
static bool elf_read_at(vfs_file_t *file, uint64_t offset, void *buffer, size_t size)
{
    vfs_file_operations_t *fops = elf_get_fops(file);

    if (fops == NULL || fops->seek == NULL || fops->read == NULL) {
        return false;
    }

    if (fops->seek(file, (long)offset, SEEK_SET) < 0) {
        return false;
    }

    int bytes_read = fops->read(file, buffer, size);
    return bytes_read == (int)size;
}

/// @brief Validate ELF64 header fields required by the loader.
static bool elf_validate_ehdr(const Elf64_Ehdr *ehdr)
{
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }

    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }

    if (ehdr->e_version != EV_CURRENT) {
        return false;
    }

    if (ehdr->e_machine != EM_X86_64) {
        return false;
    }

    if (ehdr->e_phnum == 0 || ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        return false;
    }

    if (ehdr->e_shnum > 0 && ehdr->e_shentsize != sizeof(Elf64_Shdr)) {
        return false;
    }

    return true;
}

/// @brief Convert ELF segment flags to VMA protection flags.
static int elf_phdr_to_prot(const Elf64_Phdr *phdr)
{
    int prot = 0;

    if (phdr->p_flags & PF_R) {
        prot |= PROT_READ;
    }
    if (phdr->p_flags & PF_W) {
        prot |= PROT_WRITE;
    }
    if (phdr->p_flags & PF_X) {
        prot |= PROT_EXEC;
    }

    return prot;
}

/// @brief Create VMAs for a loadable segment (file-backed and BSS tail).
static int elf_map_segment(task_t *task, vfs_file_t *file, const Elf64_Phdr *phdr)
{
    if (phdr->p_memsz == 0) {
        return 0;
    }

    if (phdr->p_filesz > phdr->p_memsz) {
        return -1;
    }

    Elf64_Addr seg_start = phdr->p_vaddr;
    Elf64_Addr seg_end = phdr->p_vaddr + phdr->p_memsz;
    Elf64_Addr file_end = phdr->p_vaddr + phdr->p_filesz;

    if (seg_end < seg_start) {
        return -1;
    }

    if (seg_end >= kHHDMOffset) {
        return -1;
    }

    Elf64_Addr page_start = align_down(seg_start);
    Elf64_Addr page_file_end = align_up(file_end);
    Elf64_Addr page_mem_end = align_up(seg_end);
    Elf64_Off page_offset = seg_start - page_start;

    if (phdr->p_offset < page_offset) {
        return -1;
    }

    Elf64_Off file_offset = phdr->p_offset - page_offset;
    int prot = elf_phdr_to_prot(phdr);

    Elf64_Addr anon_start = page_start;
    if (phdr->p_filesz > 0) {
        vma_t *file_vma = vma_create(page_start, page_file_end, prot, MAP_PRIVATE, file, file_offset);
        if (file_vma == NULL) {
            return -1;
        }
        vma_add(task, file_vma);
        anon_start = page_file_end;
    }

    if (anon_start < page_mem_end) {
        vma_t *anon_vma = vma_create(anon_start, page_mem_end, prot, MAP_PRIVATE | MAP_ANONYMOUS, NULL, 0);
        if (anon_vma == NULL) {
            return -1;
        }
        vma_add(task, anon_vma);
    }

    return 0;
}

/// @brief Populate a task from an open ELF file.
int elf_load_from_file(task_t *task, vfs_file_t *file)
{
    if (task == NULL || file == NULL) {
        return -1;
    }

    Elf64_Ehdr ehdr;
    if (!elf_read_at(file, 0, &ehdr, sizeof(ehdr))) {
        return -1;
    }

    if (!elf_validate_ehdr(&ehdr)) {
        return -1;
    }

    size_t phdr_bytes = ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs = kmalloc(phdr_bytes);
    if (phdrs == NULL) {
        return -1;
    }

    if (!elf_read_at(file, ehdr.e_phoff, phdrs, phdr_bytes)) {
        kfree(phdrs);
        return -1;
    }

    for (Elf64_Half i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) {
            continue;
        }

        if (elf_map_segment(task, file, &phdrs[i]) != 0) {
            kfree(phdrs);
            return -1;
        }
    }

    kfree(phdrs);

    elf_image_t *image = kmalloc(sizeof(*image));
    if (image == NULL) {
        return -1;
    }

    image->file = file;
    image->ehdr = ehdr;
    image->shdrs = NULL;
    image->shstrtab = NULL;
    image->shstrtab_size = 0;

    if (ehdr.e_shnum > 0 && ehdr.e_shentsize == sizeof(Elf64_Shdr)) {
        size_t shdr_bytes = ehdr.e_shnum * sizeof(Elf64_Shdr);
        Elf64_Shdr *shdrs = kmalloc(shdr_bytes);
        if (shdrs == NULL) {
            kfree(image);
            return -1;
        }

        if (!elf_read_at(file, ehdr.e_shoff, shdrs, shdr_bytes)) {
            kfree(shdrs);
            kfree(image);
            return -1;
        }

        image->shdrs = shdrs;

        if (ehdr.e_shstrndx != SHN_UNDEF && ehdr.e_shstrndx < ehdr.e_shnum) {
            Elf64_Shdr *shstr = &shdrs[ehdr.e_shstrndx];
            if (shstr->sh_type == SHT_STRTAB && shstr->sh_size > 0) {
                char *shstrtab = kmalloc(shstr->sh_size);
                if (shstrtab != NULL) {
                    if (elf_read_at(file, shstr->sh_offset, shstrtab, shstr->sh_size)) {
                        image->shstrtab = shstrtab;
                        image->shstrtab_size = shstr->sh_size;
                    } else {
                        kfree(shstrtab);
                    }
                }
            }
        }
    }

    task->elf = image;
    task->entryPoint = ehdr.e_entry;
    if (task->threads != NULL) {
        task->threads->regs.RIP = ehdr.e_entry;
    }

    return 0;
}

/// @brief Open an ELF by path and populate a task from it.
int elf_load_from_path(task_t *task, const char *path)
{
    if (task == NULL || path == NULL || kRootFilesystem == NULL) {
        return -1;
    }

    vfs_file_t *file = NULL;
    if (kRootFilesystem->fops == NULL || kRootFilesystem->fops->open == NULL) {
        return -1;
    }

    if (kRootFilesystem->fops->open(&file, path, "r", kRootFilesystem) != 0) {
        return -1;
    }

    return elf_load_from_file(task, file);
}
