#include "elf_loader.h"
#include "serial_logging.h"   // printd (elf_can_load traces why a spawn was refused)
#include <stdbool.h>

#include "CONFIG.h"
#include "memory/kmalloc.h"
#include "memory/memset.h"
#include "memory/mmap.h"
#include "memory/vma.h"
#include "paging.h"

// NOTRACE's reach: when stack traces are off, no image pays for .symtab.
extern bool kEnableStackTrace;

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
bool elf_read_at(vfs_file_t *file, uint64_t offset, void *buffer, size_t size)
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
        // The file supplies bytes only up to file_end (p_vaddr + p_filesz).  When
        // that lands mid-page, page_file_end rounds up past it and the remainder of
        // that final page is BSS: cap file_size at the real file extent so the
        // fault path zero-fills the tail rather than reading stale file bytes.
        file_vma->file_size = file_end - page_start;
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

/// @brief Compute page-aligned segment ranges (vaddr_off/pages/prot) and the
/// total page span for every PT_LOAD segment, WITHOUT allocating or reading
/// anything. Used by shared_object.c to size a lazily-populated per-page
/// cache and to build the VMAs a task maps a dynamically-linked image
/// through — the actual page contents are read on demand, per page, by
/// elf_read_page() below (called from the page-fault path, not here).
int elf_compute_segment_ranges(const Elf64_Phdr *phdrs, Elf64_Half phnum,
                                Elf64_Addr *out_vaddr_base,
                                size_t *out_total_pages,
                                elf_segment_range_t *out_segs, size_t max_segs, size_t *out_seg_count)
{
    Elf64_Addr max_end = 0;
    // The lowest page-aligned PT_LOAD vaddr. ~0 as the "nothing seen yet"
    // sentinel because a legitimate base of 0 is exactly what an ET_DYN image
    // has, so 0 cannot double as "unset". See the header for why the span is
    // measured from here and not from vaddr 0.
    Elf64_Addr min_start = (Elf64_Addr)~0UL;
    size_t seg_count = 0;

    for (Elf64_Half i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) {
            continue;
        }
        if (phdrs[i].p_filesz > phdrs[i].p_memsz || seg_count >= max_segs) {
            return -1;
        }

        Elf64_Addr seg_start = phdrs[i].p_vaddr;
        Elf64_Addr seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        Elf64_Addr page_start = align_down(seg_start);
        Elf64_Addr page_mem_end = align_up(seg_end);

        if (page_mem_end > max_end) {
            max_end = page_mem_end;
        }
        if (page_start < min_start) {
            min_start = page_start;
        }

        out_segs[seg_count].vaddr_off = page_start;
        out_segs[seg_count].pages = (page_mem_end - page_start) / PAGE_SIZE;
        out_segs[seg_count].prot = elf_phdr_to_prot(&phdrs[i]);
        seg_count++;
    }

    if (seg_count == 0 || max_end <= min_start) {
        return -1;   // no loadable content, or a nonsensical span
    }

    *out_vaddr_base = min_start;
    *out_total_pages = (max_end - min_start) / PAGE_SIZE;
    *out_seg_count = seg_count;
    return 0;
}

/// @brief Read ONE page's worth of content for a dynamically-linked image,
/// given its raw (not page-aligned) PT_LOAD table. `page_vaddr` is the page's
/// own LINK-TIME virtual address — the address the linker put in the program
/// headers, before any load bias. (It used to be a page INDEX counted from
/// vaddr 0, which quietly assumed every dynamically-linked image was ET_DYN
/// and based at zero; passing the vaddr outright is both honest about what
/// the phdr comparisons below actually need and correct for a non-PIE ET_EXEC
/// image, whose vaddrs start wherever it was linked.) `dest` must already be
/// zeroed (PAGE_SIZE bytes) — this only writes the file-backed portion, if
/// any, that overlaps this page; anything past a segment's p_filesz (BSS)
/// or outside every segment entirely is correctly left zero.
bool elf_read_page(vfs_file_t *file, const Elf64_Phdr *phdrs, Elf64_Half phnum,
                    Elf64_Addr page_vaddr, void *dest)
{
    for (Elf64_Half i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) {
            continue;
        }

        Elf64_Addr seg_start = phdrs[i].p_vaddr;
        Elf64_Addr seg_end = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (page_vaddr + PAGE_SIZE <= seg_start || page_vaddr >= seg_end) {
            continue;  // this segment doesn't cover this page at all
        }

        Elf64_Addr file_start = seg_start;
        Elf64_Addr file_end = seg_start + phdrs[i].p_filesz;
        Elf64_Addr overlap_start = page_vaddr > file_start ? page_vaddr : file_start;
        Elf64_Addr overlap_end = (page_vaddr + PAGE_SIZE) < file_end ? (page_vaddr + PAGE_SIZE) : file_end;

        if (overlap_start < overlap_end) {
            Elf64_Off file_offset = phdrs[i].p_offset + (overlap_start - seg_start);
            uint8_t *page_dest = (uint8_t *)dest + (overlap_start - page_vaddr);
            if (!elf_read_at(file, file_offset, page_dest, overlap_end - overlap_start)) {
                return false;
            }
        }
        // Deliberately NO early return here: two PT_LOAD segments sharing a
        // page is a legal layout (small p_align, custom linker scripts), and
        // stopping at the first covering segment would silently leave the
        // second segment's bytes zero — keep scanning so every segment that
        // overlaps this page contributes its slice.
    }

    return true;  // any part not covered by a PT_LOAD segment stays all zero
}

/// @brief Translate a "linked" virtual address (as it appears in dynamic
/// section tags, relative to the object's own vaddr space) to a file offset,
/// by finding the PT_LOAD segment that covers it. Dynamic section addresses
/// are never file offsets directly — this mirrors what a real dynamic linker
/// does when it hasn't mapped the file yet.
static bool elf_vaddr_to_offset(const Elf64_Phdr *phdrs, Elf64_Half phnum,
                                 Elf64_Addr vaddr, Elf64_Off *out_offset)
{
    for (Elf64_Half i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) {
            continue;
        }
        if (vaddr >= phdrs[i].p_vaddr && vaddr < phdrs[i].p_vaddr + phdrs[i].p_filesz) {
            *out_offset = phdrs[i].p_offset + (vaddr - phdrs[i].p_vaddr);
            return true;
        }
    }
    return false;
}

/// @brief Parse a PT_DYNAMIC segment (if present) into elf_image_t's dynamic
/// linking fields: raw Elf64_Dyn array, resolved symtab/strtab/rela/jmprel,
/// and DT_NEEDED library names. A no-op (returns true) for static images.
static bool elf_parse_dynamic(elf_image_t *image, vfs_file_t *file,
                               const Elf64_Phdr *phdrs, Elf64_Half phnum)
{
    const Elf64_Phdr *dyn_phdr = NULL;
    for (Elf64_Half i = 0; i < phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn_phdr = &phdrs[i];
            break;
        }
    }
    if (dyn_phdr == NULL) {
        return true;
    }

    size_t dyn_count = dyn_phdr->p_filesz / sizeof(Elf64_Dyn);
    Elf64_Dyn *dynamic = kmalloc(dyn_phdr->p_filesz);
    if (dynamic == NULL || !elf_read_at(file, dyn_phdr->p_offset, dynamic, dyn_phdr->p_filesz)) {
        kfree(dynamic);
        return false;
    }

    // First pass: locate the tables we care about. DT_NEEDED is resolved in
    // a second pass below since it needs strtab, which we haven't read yet.
    Elf64_Addr strtab_addr = 0, hash_addr = 0, symtab_addr = 0, rela_addr = 0, jmprel_addr = 0;
    Elf64_Xword strtab_size = 0, rela_size = 0, jmprel_size = 0;

    for (size_t i = 0; i < dyn_count && dynamic[i].d_tag != DT_NULL; i++) {
        switch (dynamic[i].d_tag) {
            case DT_STRTAB:   strtab_addr = dynamic[i].d_un.d_ptr; break;
            case DT_HASH:     hash_addr   = dynamic[i].d_un.d_ptr; break;
            case DT_SYMTAB:   symtab_addr = dynamic[i].d_un.d_ptr; break;
            case DT_STRSZ:    strtab_size = dynamic[i].d_un.d_val; break;
            case DT_RELA:     rela_addr   = dynamic[i].d_un.d_ptr; break;
            case DT_RELASZ:   rela_size   = dynamic[i].d_un.d_val; break;
            case DT_JMPREL:   jmprel_addr = dynamic[i].d_un.d_ptr; break;
            case DT_PLTRELSZ: jmprel_size = dynamic[i].d_un.d_val; break;
            default: break;
        }
    }

    image->is_dynamic = true;
    image->dynamic = dynamic;
    image->dynamic_count = dyn_count;

    if (strtab_addr != 0 && strtab_size != 0) {
        Elf64_Off off;
        char *strtab = NULL;
        if (!elf_vaddr_to_offset(phdrs, phnum, strtab_addr, &off) ||
            (strtab = kmalloc(strtab_size)) == NULL ||
            !elf_read_at(file, off, strtab, strtab_size)) {
            kfree(strtab);
            return false;
        }
        image->strtab = strtab;
        image->strtab_size = strtab_size;
    }

    // DT_SYMTAB has no matching *SZ tag of its own — the symbol count comes
    // from DT_HASH's nchain (the hash table's second word), same as a real
    // dynamic linker derives it.
    size_t symtab_count = 0;
    if (hash_addr != 0) {
        Elf64_Off off;
        uint32_t hash_hdr[2];
        if (!elf_vaddr_to_offset(phdrs, phnum, hash_addr, &off) ||
            !elf_read_at(file, off, hash_hdr, sizeof(hash_hdr))) {
            return false;
        }
        symtab_count = hash_hdr[1];
    }

    if (symtab_addr != 0 && symtab_count != 0) {
        Elf64_Off off;
        size_t bytes = symtab_count * sizeof(Elf64_Sym);
        Elf64_Sym *symtab = NULL;
        if (!elf_vaddr_to_offset(phdrs, phnum, symtab_addr, &off) ||
            (symtab = kmalloc(bytes)) == NULL ||
            !elf_read_at(file, off, symtab, bytes)) {
            kfree(symtab);
            return false;
        }
        image->symtab = symtab;
        image->symtab_count = symtab_count;
    }

    if (rela_addr != 0 && rela_size != 0) {
        Elf64_Off off;
        Elf64_Rela *rela = NULL;
        if (!elf_vaddr_to_offset(phdrs, phnum, rela_addr, &off) ||
            (rela = kmalloc(rela_size)) == NULL ||
            !elf_read_at(file, off, rela, rela_size)) {
            kfree(rela);
            return false;
        }
        image->rela = rela;
        image->rela_count = rela_size / sizeof(Elf64_Rela);
    }

    if (jmprel_addr != 0 && jmprel_size != 0) {
        Elf64_Off off;
        Elf64_Rela *jmprel = NULL;
        if (!elf_vaddr_to_offset(phdrs, phnum, jmprel_addr, &off) ||
            (jmprel = kmalloc(jmprel_size)) == NULL ||
            !elf_read_at(file, off, jmprel, jmprel_size)) {
            kfree(jmprel);
            return false;
        }
        image->jmprel = jmprel;
        image->jmprel_count = jmprel_size / sizeof(Elf64_Rela);
    }

    // DT_NEEDED values are string-table byte offsets — resolve them now that
    // strtab is loaded. Names point directly into image->strtab, no copy.
    for (size_t i = 0; i < dyn_count && dynamic[i].d_tag != DT_NULL; i++) {
        if (dynamic[i].d_tag != DT_NEEDED || image->needed_count >= ELF_MAX_NEEDED) {
            continue;
        }
        if (image->strtab != NULL && dynamic[i].d_un.d_val < image->strtab_size) {
            image->needed[image->needed_count++] = image->strtab + dynamic[i].d_un.d_val;
        }
    }

    return true;
}

/// @brief Free an elf_image_t and every table it owns. Safe on a partially
/// populated image (elf_parse_image zeroes all table pointers before any of
/// them are filled in, so kfree of a never-populated table is a NULL no-op).
/// Does NOT close image->file — the file's lifetime belongs to the caller
/// (the static path keeps it open for file-backed VMAs; shared_object.c
/// keeps it open for lazy per-page reads).
void elf_image_free(elf_image_t *image)
{
    if (image == NULL) {
        return;
    }
    kfree(image->shdrs);
    kfree(image->shstrtab);
    kfree(image->tracesyms);
    kfree(image->tracestr);
    kfree(image->dynamic);
    kfree(image->symtab);
    kfree(image->strtab);
    kfree(image->rela);
    kfree(image->jmprel);
    kfree(image);
}

/// @brief Parse an ELF64 file's header, program headers, section headers,
/// and (if present) dynamic-linking section into a freshly allocated
/// elf_image_t. Does not touch any task or create any VMAs — shared by
/// elf_load_from_file (which maps PT_LOAD segments into a task) and
/// shared_object.c (which manages its own physical pages instead).
/// *out_phdrs is a kmalloc'd copy of the raw PT_LOAD table; the caller owns
/// it and must kfree it.
int elf_parse_image(vfs_file_t *file, elf_image_t **out_image,
                     Elf64_Phdr **out_phdrs, Elf64_Half *out_phnum)
{
    if (file == NULL || out_image == NULL || out_phdrs == NULL || out_phnum == NULL) {
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

    elf_image_t *image = kmalloc(sizeof(*image));
    if (image == NULL) {
        kfree(phdrs);
        return -1;
    }

    image->file = file;
    image->ehdr = ehdr;
    image->shdrs = NULL;
    image->shstrtab = NULL;
    image->shstrtab_size = 0;
    image->is_dynamic = false;
    image->dynamic = NULL;
    image->dynamic_count = 0;
    image->symtab = NULL;
    image->symtab_count = 0;
    image->strtab = NULL;
    image->strtab_size = 0;
    image->rela = NULL;
    image->rela_count = 0;
    image->jmprel = NULL;
    image->jmprel_count = 0;
    image->needed_count = 0;
    image->tracesyms = NULL;
    image->tracesym_count = 0;
    image->tracestr = NULL;
    image->tracestr_size = 0;

    if (ehdr.e_shnum > 0 && ehdr.e_shentsize == sizeof(Elf64_Shdr)) {
        size_t shdr_bytes = ehdr.e_shnum * sizeof(Elf64_Shdr);
        Elf64_Shdr *shdrs = kmalloc(shdr_bytes);
        if (shdrs == NULL) {
            kfree(image);
            kfree(phdrs);
            return -1;
        }

        if (!elf_read_at(file, ehdr.e_shoff, shdrs, shdr_bytes)) {
            kfree(shdrs);
            kfree(image);
            kfree(phdrs);
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

        // .symtab + its string table, for naming addresses when this program
        // faults (stack_trace.c). Both are OPTIONAL in every sense: a stripped
        // binary has no SHT_SYMTAB, NOTRACE skips the work entirely, and any
        // failure here leaves the pointers NULL and simply costs the program
        // its names — never its load. A missing symbol table must not be a
        // reason a program refuses to run.
        if (kEnableStackTrace) {
            for (Elf64_Half i = 0; i < ehdr.e_shnum; i++) {
                if (shdrs[i].sh_type != SHT_SYMTAB || shdrs[i].sh_size == 0) {
                    continue;
                }
                // sh_link names the SHT_STRTAB these symbols index into — the
                // one thing that must be right, since a symbol name is an
                // offset into it and nothing else can supply that.
                if (shdrs[i].sh_link == SHN_UNDEF || shdrs[i].sh_link >= ehdr.e_shnum) {
                    break;
                }
                Elf64_Shdr *symsec = &shdrs[i];
                Elf64_Shdr *strsec = &shdrs[shdrs[i].sh_link];
                if (strsec->sh_type != SHT_STRTAB || strsec->sh_size == 0 ||
                    symsec->sh_entsize != sizeof(Elf64_Sym)) {
                    break;
                }

                Elf64_Sym *syms = kmalloc(symsec->sh_size);
                char *strs = kmalloc(strsec->sh_size);
                if (syms != NULL && strs != NULL &&
                    elf_read_at(file, symsec->sh_offset, syms, symsec->sh_size) &&
                    elf_read_at(file, strsec->sh_offset, strs, strsec->sh_size)) {
                    image->tracesyms = syms;
                    image->tracesym_count = symsec->sh_size / sizeof(Elf64_Sym);
                    image->tracestr = strs;
                    image->tracestr_size = strsec->sh_size;
                } else {
                    // kfree(NULL) PANICS in os64 — guard both, every time.
                    if (syms != NULL) kfree(syms);
                    if (strs != NULL) kfree(strs);
                }
                break;   // one .symtab per image; ELF allows no more
            }
        }
    }

    if (!elf_parse_dynamic(image, file, phdrs, ehdr.e_phnum)) {
        // elf_image_free (not inline kfrees) — elf_parse_dynamic can fail
        // partway through, after some of dynamic/strtab/symtab/rela/jmprel
        // were already populated; freeing only shdrs/shstrtab leaked those.
        elf_image_free(image);
        kfree(phdrs);
        return -1;
    }

    *out_image = image;
    *out_phdrs = phdrs;
    *out_phnum = ehdr.e_phnum;
    return 0;
}

/// @brief Populate a task from an open ELF file.
int elf_load_from_file(task_t *task, vfs_file_t *file)
{
    if (task == NULL || file == NULL) {
        return -1;
    }

    elf_image_t *image = NULL;
    Elf64_Phdr *phdrs = NULL;
    Elf64_Half phnum = 0;
    if (elf_parse_image(file, &image, &phdrs, &phnum) != 0) {
        return -1;
    }

    for (Elf64_Half i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) {
            continue;
        }

        if (elf_map_segment(task, file, &phdrs[i]) != 0) {
            kfree(phdrs);
            elf_image_free(image);
            return -1;
        }
    }

    kfree(phdrs);

    task->elf = image;
    task->entryPoint = image->ehdr.e_entry;
    if (task->threads != NULL) {
        task->threads->regs.RIP = image->ehdr.e_entry;
    }

    return 0;
}

/// @brief Open an ELF by path and populate a task from it.
int elf_load_from_path(task_t *task, const char *path)
{
    if (task == NULL || path == NULL) {
        return -1;
    }

    // Mount-routed: programs can live on ANY mounted filesystem now —
    // "/fat/bin/ls" runs from the FAT volume when ext2 is root, and vice
    // versa. (A NULL from the resolver means nothing is mounted at all,
    // the same gate kRootFilesystem==NULL used to provide.)
    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount(path, &tail);
    if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL) {
        return -1;
    }

    vfs_file_t *file = NULL;
    if (fs->fops->open(&file, tail, "r", fs) != 0) {
        return -1;
    }

    return elf_load_from_file(task, file);
}

/// @brief Peek at an ELF file's PT_DYNAMIC presence without fully loading
/// it. task_create uses this to choose between the demand-paged static path
/// (elf_load_from_path) and the eager dynamic-linking path
/// (elf_resolve_dynamic_dependencies, task.c) — the two are mutually
/// exclusive per task, so this check happens once up front rather than
/// letting either path discover it's the wrong one partway through.
// Could `path` actually be run? Opens it and validates the ELF64 header, and
// allocates NOTHING — which is the whole point: task_create calls this BEFORE it
// builds a task, so a bad path costs nothing and leaks nothing.
//
// WHY THIS EXISTS: task_create used to PANIC ("Failed to load ELF") when the
// image wouldn't load. That meant ring 3 could take the kernel down with a
// TYPO — one mistyped filename at the husk prompt killed the whole OS. It also
// died on a path that exists but isn't a program (try /partition_info, a text
// file). Neither is a kernel error. Both are just "no", and "no" is an answer a
// shell is perfectly capable of printing.
bool elf_can_load(const char *path)
{
    if (path == NULL) {
        return false;
    }

    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount(path, &tail);
    if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL) {
        return false;
    }

    vfs_file_t *file = NULL;
    if (fs->fops->open(&file, tail, "r", fs) != 0) {
        printd(DEBUG_TASK, "elf_can_load: no such file: %s\n", path);
        return false;   // the typo case
    }

    // It opened — but is it a PROGRAM? A text file opens just fine.
    Elf64_Ehdr ehdr;
    bool loadable = elf_read_at(file, 0, &ehdr, sizeof(ehdr)) && elf_validate_ehdr(&ehdr);
    if (!loadable) {
        printd(DEBUG_TASK, "elf_can_load: not a loadable ELF64: %s\n", path);
    }

    if (fs->fops->close != NULL) {
        fs->fops->close(file);
    }

    return loadable;
}

bool elf_is_dynamic(const char *path)
{
    if (path == NULL) {
        return false;
    }

    const char *tail = NULL;
    vfs_filesystem_t *fs = vfs_resolve_mount(path, &tail);
    if (fs == NULL || fs->fops == NULL || fs->fops->open == NULL) {
        return false;
    }

    vfs_file_t *file = NULL;
    if (fs->fops->open(&file, tail, "r", fs) != 0) {
        return false;
    }

    Elf64_Ehdr ehdr;
    bool dynamic = false;
    if (elf_read_at(file, 0, &ehdr, sizeof(ehdr)) && elf_validate_ehdr(&ehdr)) {
        size_t phdr_bytes = ehdr.e_phnum * sizeof(Elf64_Phdr);
        Elf64_Phdr *phdrs = kmalloc(phdr_bytes);
        if (phdrs != NULL && elf_read_at(file, ehdr.e_phoff, phdrs, phdr_bytes)) {
            for (Elf64_Half i = 0; i < ehdr.e_phnum; i++) {
                if (phdrs[i].p_type == PT_DYNAMIC) {
                    dynamic = true;
                    break;
                }
            }
        }
        kfree(phdrs);
    }

    if (fs->fops->close != NULL) {
        fs->fops->close(file);
    }

    return dynamic;
}
