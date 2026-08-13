// symbols.c — names for kernel addresses. Doctrine and the safety contract
// live in symbols.h; this file is the table builder and the lookup.
//
// The shape deliberately mirrors stack_trace.c's ring-3 sym_for_address():
// same .symtab walk, same "+0x" offset convention, same refuse-don't-guess
// posture on malformed indexes. Ring 3 reads its symbols from the image the
// ELF loader kept; ring 0 reads them from the kernel file Limine kept. One
// convention, two tables, no drift.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "symbols.h"
#include "elf.h"
#include "kmalloc.h"
#include "memcpy.h"
#include "serial_logging.h"
#include "BasicRenderer.h"   // printf — the boot line belongs on the glass too
#include "CONFIG.h"

// Stashed by limine_boot_entry_point BEFORE the CR3 switch, because the
// response structs themselves live in bootloader-reclaimable memory that is
// only reachable through Limine's own tables. The blob these point at is
// re-mapped into the kernel's tables by init_os64_paging_tables (same recipe
// as the font and PCI-ID modules), so it is still readable when we run.
extern uint64_t kKernelFileAddress;
extern uint64_t kKernelFileSize;

// One table row. `end` is one past the last byte the name owns —
// floor-to-next-symbol (see the header), capped at the symbol's own
// section end so a gap between executable sections never inherits a name.
typedef struct {
	uint64_t addr;
	uint64_t end;
	uint32_t name;   // offset into kSymStrings
} sym_entry_t;

static sym_entry_t *kSymEntries;
static size_t       kSymCount;
static char        *kSymStrings;
static size_t       kSymStringsSize;

// Is this symbol one we want a row for? Executable section, real address,
// a function or an asm label, and a non-empty name that fits the strtab.
static bool sym_eligible(const Elf64_Sym *s, const Elf64_Shdr *sh,
                         uint16_t shnum, size_t strsz)
{
	unsigned char type = ELF64_ST_TYPE(s->st_info);

	if (s->st_shndx == SHN_UNDEF || s->st_shndx >= shnum)
		return false;
	if (!(sh[s->st_shndx].sh_flags & SHF_EXECINSTR))
		return false;
	if (s->st_value == 0)
		return false;
	if (type != STT_FUNC && type != STT_NOTYPE)
		return false;
	if (s->st_name == 0 || s->st_name >= strsz)
		return false;
	return true;
}

void symbols_init(void)
{
	if (kKernelFileAddress == 0 || kKernelFileSize < sizeof(Elf64_Ehdr))
	{
		printd(DEBUG_EXCEPTIONS, "symbols: no kernel file from the bootloader — kernel addresses stay hex\n");
		return;
	}

	const uint8_t *file = (const uint8_t *)kKernelFileAddress;
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;

	// Every offset below is bounds-checked against the file size before use.
	// This is OUR OWN kernel binary, so none of these should ever fire — but
	// "should never" is what the reporting arc keeps finding in the morgue,
	// and a symbolizer that walks off its buffer poisons the very reports it
	// exists to improve.
	if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F' || eh->e_ident[4] != 2)
	{
		printd(DEBUG_EXCEPTIONS, "symbols: kernel file is not ELF64 — kernel addresses stay hex\n");
		return;
	}
	if (eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shnum == 0 ||
	    eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > kKernelFileSize)
	{
		printd(DEBUG_EXCEPTIONS, "symbols: section headers out of bounds — kernel addresses stay hex\n");
		return;
	}

	const Elf64_Shdr *sh = (const Elf64_Shdr *)(file + eh->e_shoff);

	// Find .symtab and its linked string table.
	const Elf64_Shdr *symtab = NULL, *strtab = NULL;
	for (uint16_t i = 0; i < eh->e_shnum; i++)
	{
		if (sh[i].sh_type == SHT_SYMTAB && sh[i].sh_link < eh->e_shnum &&
		    sh[sh[i].sh_link].sh_type == SHT_STRTAB)
		{
			symtab = &sh[i];
			strtab = &sh[sh[i].sh_link];
			break;
		}
	}
	if (symtab == NULL ||
	    symtab->sh_entsize != sizeof(Elf64_Sym) || symtab->sh_size == 0 ||
	    symtab->sh_offset + symtab->sh_size > kKernelFileSize ||
	    strtab->sh_size == 0 ||
	    strtab->sh_offset + strtab->sh_size > kKernelFileSize)
	{
		printd(DEBUG_EXCEPTIONS, "symbols: kernel was stripped or tables malformed — addresses stay hex\n");
		return;
	}

	const Elf64_Sym *syms = (const Elf64_Sym *)(file + symtab->sh_offset);
	size_t nsyms = symtab->sh_size / sizeof(Elf64_Sym);
	const char *strs = (const char *)(file + strtab->sh_offset);
	size_t strsz = strtab->sh_size;

	// Pass 1: count the rows so both tables are single exact allocations.
	size_t count = 0;
	for (size_t i = 0; i < nsyms; i++)
		if (sym_eligible(&syms[i], sh, eh->e_shnum, strsz))
			count++;
	if (count == 0)
	{
		printd(DEBUG_EXCEPTIONS, "symbols: no executable symbols found — addresses stay hex\n");
		return;
	}

	sym_entry_t *entries = kmalloc(count * sizeof(sym_entry_t));
	char *strings = kmalloc(strsz);
	if (entries == NULL || strings == NULL)
	{
		// kfree(NULL) is legal here (the graveyard shift fixed it), so no
		// need to distinguish which of the two failed.
		kfree(entries);
		kfree(strings);
		printd(DEBUG_EXCEPTIONS, "symbols: out of memory for tables — addresses stay hex\n");
		return;
	}

	// The whole strtab, copied once: entry name offsets stay valid verbatim,
	// and the file could in principle be unmapped afterward (it is kept —
	// .debug_line lives there and the file:line slice will want it).
	memcpy(strings, strs, strsz);

	// Pass 2: fill. `end` starts as the SECTION end; the sort below tightens
	// it to the next symbol.
	size_t n = 0;
	for (size_t i = 0; i < nsyms; i++)
	{
		const Elf64_Sym *s = &syms[i];
		if (!sym_eligible(s, sh, eh->e_shnum, strsz))
			continue;
		entries[n].addr = s->st_value;
		entries[n].end  = sh[s->st_shndx].sh_addr + sh[s->st_shndx].sh_size;
		entries[n].name = (uint32_t)s->st_name;
		n++;
	}

	// Sort by address — Shell sort: no recursion, no allocation, and ~1400
	// rows is nothing. (Ciura's gap sequence, clipped to what the count needs.)
	static const size_t gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
	for (size_t g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
	{
		size_t gap = gaps[g];
		for (size_t i = gap; i < n; i++)
		{
			sym_entry_t tmp = entries[i];
			size_t j = i;
			for (; j >= gap && entries[j - gap].addr > tmp.addr; j -= gap)
				entries[j] = entries[j - gap];
			entries[j] = tmp;
		}
	}

	// Tighten each row's end to the next symbol's start (floor-to-next-symbol),
	// still capped by its own section end from pass 2 — a label owns the bytes
	// down to the next label, never past its section.
	for (size_t i = 0; i + 1 < n; i++)
		if (entries[i + 1].addr < entries[i].end)
			entries[i].end = entries[i + 1].addr;

	// Publish only after the tables are complete, count last — a lookup that
	// races boot sees either nothing or everything.
	kSymStrings = strings;
	kSymStringsSize = strsz;
	kSymEntries = entries;
	__asm__ volatile("" ::: "memory");
	kSymCount = n;

	printf("Kernel symbols: %lu names from .symtab (%lu KB of strings)\n",
	       (uint64_t)n, (uint64_t)(strsz / 1024));
	printd(DEBUG_EXCEPTIONS, "symbols: %lu names loaded, %lu bytes of strings\n",
	       (uint64_t)n, (uint64_t)strsz);
}

const char *symbols_for_address(uint64_t addr, uint64_t *offset)
{
	// No table, no answer — and the caller prints hex, which is exactly what
	// it printed before this file existed.
	if (kSymCount == 0 || kSymEntries == NULL || kSymStrings == NULL)
		return NULL;
	if (addr < kSymEntries[0].addr)
		return NULL;

	// Binary search for the floor entry: greatest addr <= target.
	size_t lo = 0, hi = kSymCount - 1;
	while (lo < hi)
	{
		size_t mid = lo + (hi - lo + 1) / 2;
		if (kSymEntries[mid].addr <= addr)
			lo = mid;
		else
			hi = mid - 1;
	}

	const sym_entry_t *e = &kSymEntries[lo];
	if (addr >= e->end)
		return NULL;   // in the gap past a section's last symbol
	if (e->name >= kSymStringsSize)
		return NULL;   // malformed index — refuse rather than run off

	if (offset != NULL)
		*offset = addr - e->addr;
	return kSymStrings + e->name;
}
