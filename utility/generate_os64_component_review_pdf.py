#!/usr/bin/env python3
from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
VENDOR_DIR = REPO_ROOT / ".vendor"
if str(VENDOR_DIR) not in sys.path:
    sys.path.insert(0, str(VENDOR_DIR))

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import LETTER
from reportlab.lib.styles import ParagraphStyle, StyleSheet1, getSampleStyleSheet
from reportlab.lib.units import inch
from reportlab.platypus import (
    PageBreak,
    Paragraph,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)

OUTPUT_PDF = REPO_ROOT / "output" / "pdf" / "os64_component_review.pdf"
PREVIEW_DIR = REPO_ROOT / "tmp" / "pdfs"


@dataclass(frozen=True)
class Component:
    name: str
    status: str
    role: str
    files: list[str]
    summary: str
    review: str


COMPONENTS = [
    Component(
        name="Boot And Entry",
        status="Core",
        role="Validate boot data and hand control to the higher-half kernel.",
        files=[
            "kernel/src/main.c",
            "kernel/src/kernel.c",
        ],
        summary=(
            "The boot path is Limine-first and intentionally direct. "
            "The entry stub installs a temporary stack, validates the bootloader "
            "responses, captures framebuffer, memory map, modules, SMP, ACPI, "
            "kernel address, and command line data, then hands off to "
            "kernel_main(). From there the kernel parses configuration, brings up "
            "serial and video output, builds its memory map and paging structures, "
            "switches to a kernel heap-backed stack, and continues into full "
            "system initialization."
        ),
        review=(
            "For a hobby kernel this is a healthy shape: the path is short, the "
            "handoff points are easy to follow, and most early assumptions are "
            "logged. It favors understandable bring-up over abstraction, which is "
            "a good trade when debugging first-boot failures."
        ),
    ),
    Component(
        name="Kernel Bring-Up And Configuration",
        status="Core",
        role="Sequence subsystem startup and apply command-line configuration.",
        files=[
            "kernel/src/kernel.c",
            "kernel/src/kernel_commandline.c",
            "kernel/include/CONFIG.h",
        ],
        summary=(
            "kernel_init() acts as the system conductor. It discovers ACPI tables, "
            "maps PCI configuration space, enumerates PCI devices, optionally "
            "starts AHCI and NVMe, detects CPU details, initializes SMP, creates "
            "the kernel task, starts idle tasks, enables the scheduler, mounts the "
            "root filesystem when ROOT= is supplied, runs tests, and finally boots "
            "a small ELF as a loader smoke test. Configuration is intentionally "
            "simple: compile-time flags in CONFIG.h and a small command-line parser "
            "toggle logging, SMP, and storage drivers."
        ),
        review=(
            "This is classic educational-kernel orchestration code: central, "
            "procedural, and easy to reason about in a debugger. The upside is "
            "clarity; the tradeoff is that subsystem boundaries are practical more "
            "than formal, which is fine at this project stage."
        ),
    ),
    Component(
        name="Memory Management",
        status="Core",
        role="Own physical allocation, virtual mapping, kernel heap, and VMAs.",
        files=[
            "kernel/src/memory/paging.c",
            "kernel/src/memory/allocator.c",
            "kernel/src/memory/kmalloc.c",
            "kernel/src/memory/vma.c",
        ],
        summary=(
            "Memory management is split into four layers. The physical allocator "
            "tracks free and used regions with a memory-status array derived from "
            "the bootloader map. The paging layer builds 4-level x86_64 page "
            "tables, maps the higher-half kernel, and exposes helpers to map, "
            "unmap, and walk entries. kmalloc() is a kernel convenience layer that "
            "allocates physical memory and maps it into the kernel's higher-half "
            "space. VMAs sit one level above that and describe anonymous or "
            "file-backed regions so task pages can be resolved lazily on fault."
        ),
        review=(
            "The design is pragmatic and readable. It clearly separates "
            "kernel-permanent allocations from task-oriented pages, which is an "
            "important conceptual win. The VMA layer is still compact and purpose-"
            "built, but it already shows the project moving beyond simple eager "
            "mapping."
        ),
    ),
    Component(
        name="Tasks And Threads",
        status="Core",
        role="Represent address spaces, execution contexts, argv, cwd, and env.",
        files=[
            "kernel/src/task.c",
            "kernel/src/thread.c",
        ],
        summary=(
            "A task owns an address space, environment, argv, cwd, stdio handles, "
            "and one or more threads. ktask is special and reuses the kernel page "
            "table directly, while later tasks get their own PML4 with the kernel "
            "upper half shared in. Threads receive guarded stacks, saved register "
            "state, and a return path into task_exit(). Task creation also wires in "
            "ELF loading when a root filesystem exists, making the task layer the "
            "bridge between filesystem objects and executable state."
        ),
        review=(
            "This is a solid hobby-OS process model: not POSIX-complete, but "
            "concrete enough to support real isolation experiments. The code pays "
            "careful attention to stack switching and CR3 changes, which is exactly "
            "where small kernels often get hurt."
        ),
    ),
    Component(
        name="Scheduler And Run State",
        status="Core",
        role="Move threads between runnable, sleep, running, and zombie queues.",
        files=[
            "kernel/src/scheduler.c",
            "kernel/src/scheduler.S",
            "kernel/src/signals.c",
        ],
        summary=(
            "Scheduling is queue-based and global in shape, with explicit lists for "
            "running, runnable, interruptible sleep, uninterruptible sleep, "
            "stopped, and zombie threads. Each CPU gets a scheduler stack, and "
            "both timer interrupts and a manual software interrupt can enter the "
            "scheduler. Signals are lightweight and mostly scheduler-facing today: "
            "SIGSLEEP and log flushing are used to move threads between sleep and "
            "runnable states."
        ),
        review=(
            "This is a deliberately understandable scheduler rather than a fully "
            "fair or feature-heavy one. That makes sense here. The code is focused "
            "on correctness of queue transitions, cross-core coordination, and "
            "debuggability instead of policy sophistication."
        ),
    ),
    Component(
        name="SMP, APIC, And Core-Local State",
        status="Core",
        role="Bring up multiple CPUs and route timer and IPI traffic.",
        files=[
            "kernel/src/smp.c",
            "kernel/src/smp_core.c",
            "kernel/src/apic.c",
        ],
        summary=(
            "The SMP code combines several discovery sources to get cores online. "
            "Limine provides CPU records, ACPI and legacy MP structures help locate "
            "APIC resources, and the kernel builds per-core state including APIC "
            "metadata, scheduler stacks, and idle tasks. Local APIC and IOAPIC "
            "registers are mapped into the kernel address space, IRQ0 is redirected "
            "through the IOAPIC, and application processors are brought into the "
            "same scheduling model as the BSP."
        ),
        review=(
            "For a hobby OS this is one of the more ambitious parts of the tree. "
            "The mix of Limine, ACPI, and Intel MP parsing reflects a practical "
            "bring-up mentality: use whichever path gets trustworthy hardware data "
            "soonest."
        ),
    ),
    Component(
        name="Interrupts, Exceptions, Syscalls, And Signals",
        status="Emerging",
        role="Handle traps, IRQs, and basic user-to-kernel control flow.",
        files=[
            "kernel/src/driver/system/idt.c",
            "kernel/src/syscall.c",
            "kernel/src/signals.c",
            "kernel/src/driver/system/exceptions/simple_exceptions.c",
        ],
        summary=(
            "The interrupt layer installs a compact IDT with handlers for core CPU "
            "exceptions, PIT and keyboard IRQs, and several SMP-specific vectors. "
            "Syscalls are present but intentionally small: the dispatch table "
            "currently exposes yield and debug logging, validates user addresses, "
            "and can switch to the kernel CR3 when a syscall needs kernel-only "
            "access. Signals are not POSIX-style process signals yet; they are a "
            "kernel coordination mechanism tied closely to the scheduler."
        ),
        review=(
            "This subsystem is clearly in the 'growing infrastructure' phase. That "
            "is not a weakness in a hobby OS. The important part is that the hooks "
            "exist and already exercise user-to-kernel transitions in a disciplined "
            "way."
        ),
    ),
    Component(
        name="ACPI And PCI Discovery",
        status="Core",
        role="Find firmware tables and enumerate PCI devices and bridges.",
        files=[
            "kernel/src/driver/system/acpi.c",
            "kernel/src/driver/system/pci.c",
            "kernel/src/driver/system/pci_lookup.c",
        ],
        summary=(
            "ACPI parsing is used to find major firmware tables such as FADT, MCFG, "
            "and MADT. The MCFG path supplies PCI ECAM space, which the PCI layer "
            "uses to read headers, identify bridges, discover functions, and store "
            "device metadata for later drivers. A small PCI ID database loaded as a "
            "boot module makes logs more readable by mapping numeric identifiers to "
            "human-friendly names."
        ),
        review=(
            "This is a practical hardware-discovery stack with a nice educational "
            "property: the code keeps the raw table walking visible instead of "
            "burying it in framework layers. That makes it much easier to learn "
            "from and debug."
        ),
    ),
    Component(
        name="Block Devices, Partitions, And Storage Drivers",
        status="Core",
        role="Probe controllers, expose block I/O, and parse partition metadata.",
        files=[
            "kernel/src/driver/block/block_device.c",
            "kernel/src/driver/block/gpt.c",
            "kernel/src/driver/filesystem/filesystem.c",
            "kernel/src/driver/system/ahci.c",
            "kernel/src/driver/system/nvme.c",
        ],
        summary=(
            "Storage is discovered from PCI outward. AHCI and NVMe probe matching "
            "controllers, map their MMIO regions, identify devices, and register "
            "block operations. The block layer assigns simple major/minor numbers "
            "and keeps a linked list of mounted block-backed filesystems. GPT "
            "parsing extracts partitions and GUIDs, then a filesystem probe sniffs "
            "common on-disk signatures such as FAT32 and ext2."
        ),
        review=(
            "The end-to-end story is strong: discover controller, build block "
            "device, read partitions, identify filesystem, mount root. That is the "
            "right level of ambition for a hobby kernel because every successful "
            "boot proves multiple layers at once."
        ),
    ),
    Component(
        name="VFS And Filesystems",
        status="Pragmatic",
        role="Adapt FAT-centric storage into a small generic file API.",
        files=[
            "kernel/src/driver/filesystem/vfs/vfs.c",
            "kernel/src/driver/filesystem/fat/fat_glue.c",
            "kernel/src/driver/filesystem/ext2/ext2.c",
        ],
        summary=(
            "The VFS layer is intentionally lightweight. A mounted filesystem gets "
            "file, directory, and block operation tables plus a mount structure and "
            "basic open-file bookkeeping. FAT32 is the practical path today: the "
            "FatFs glue adapts the generic VFS API to sector reads, writes, seeks, "
            "directory iteration, and timestamps. ext2 support exists mainly as an "
            "exploration and parsing path rather than the primary mounted-root flow."
        ),
        review=(
            "This is a very reasonable hobby-OS filesystem strategy. FAT gives the "
            "project a fast path to bootable disks and easy interoperability, while "
            "ext2 acts as a learning and expansion target instead of a forced early "
            "dependency."
        ),
    ),
    Component(
        name="ELF Loading And Demand Paging",
        status="Emerging",
        role="Turn filesystem objects into lazily populated process images.",
        files=[
            "kernel/src/elf_loader.c",
            "kernel/src/memory/vma.c",
            "kernel/test/elf/serial_ping.S",
        ],
        summary=(
            "The ELF loader validates ELF64 headers, reads program headers, and "
            "creates VMAs instead of eagerly copying every segment into RAM. File-"
            "backed pages and anonymous BSS tails are represented separately, and a "
            "fault later resolves the backing page through the VMA layer. The "
            "loader also records section metadata for richer introspection and sets "
            "the initial RIP for the task's main thread."
        ),
        review=(
            "This is one of the most interesting subsystems in the kernel because it "
            "pushes the project from 'can launch code' toward 'can model process "
            "memory'. For a hobby OS, using VMAs plus a test ELF is exactly the "
            "right scale of experiment."
        ),
    ),
    Component(
        name="Logging, Diagnostics, And Test Harness",
        status="Pragmatic",
        role="Provide serial-first observability and simple subsystem tests.",
        files=[
            "kernel/src/serial_logging.c",
            "kernel/src/tests.c",
            "kernel/include/test_framework.h",
        ],
        summary=(
            "The kernel leans heavily on serial logging and compile-time debug "
            "flags. printd() gives subsystem-scoped diagnostics, while printf() "
            "pushes messages to the console path unconditionally. The test harness "
            "runs preboot and postboot checks, then exercises storage and VFS code "
            "using simple file creation, reading, and directory traversal tests."
        ),
        review=(
            "For a personal OS project, this is exactly the right bias. Logs and "
            "small targeted tests are far more valuable than elaborate frameworks "
            "when the machine can still fail before multitasking is stable."
        ),
    ),
]


def build_styles() -> StyleSheet1:
    styles = getSampleStyleSheet()
    styles.add(
        ParagraphStyle(
            name="DocTitle",
            parent=styles["Title"],
            fontName="Helvetica-Bold",
            fontSize=24,
            leading=28,
            alignment=TA_CENTER,
            textColor=colors.HexColor("#17324D"),
            spaceAfter=10,
        )
    )
    styles.add(
        ParagraphStyle(
            name="DocSubtitle",
            parent=styles["BodyText"],
            fontName="Helvetica",
            fontSize=11,
            leading=15,
            alignment=TA_CENTER,
            textColor=colors.HexColor("#4C5D6C"),
            spaceAfter=6,
        )
    )
    styles.add(
        ParagraphStyle(
            name="SectionTitle",
            parent=styles["Heading2"],
            fontName="Helvetica-Bold",
            fontSize=15,
            leading=18,
            textColor=colors.HexColor("#17324D"),
            spaceBefore=10,
            spaceAfter=8,
        )
    )
    styles.add(
        ParagraphStyle(
            name="BodySmall",
            parent=styles["BodyText"],
            fontName="Helvetica",
            fontSize=9.5,
            leading=13,
            textColor=colors.HexColor("#243746"),
            alignment=TA_LEFT,
            spaceAfter=6,
        )
    )
    styles.add(
        ParagraphStyle(
            name="Meta",
            parent=styles["BodyText"],
            fontName="Courier",
            fontSize=8.5,
            leading=11,
            textColor=colors.HexColor("#5D6875"),
            spaceAfter=6,
        )
    )
    styles.add(
        ParagraphStyle(
            name="TableBody",
            parent=styles["BodyText"],
            fontName="Helvetica",
            fontSize=8.4,
            leading=10.2,
            textColor=colors.HexColor("#243746"),
            alignment=TA_LEFT,
            spaceAfter=0,
        )
    )
    styles.add(
        ParagraphStyle(
            name="Label",
            parent=styles["BodyText"],
            fontName="Helvetica-Bold",
            fontSize=9.5,
            leading=13,
            textColor=colors.HexColor("#8A5A15"),
            spaceAfter=2,
        )
    )
    return styles


def draw_page_chrome(canvas, doc) -> None:
    width, height = LETTER
    canvas.saveState()

    canvas.setFillColor(colors.HexColor("#F7F4EE"))
    canvas.rect(0, 0, width, height, fill=1, stroke=0)

    canvas.setFillColor(colors.HexColor("#17324D"))
    canvas.rect(0, height - 0.45 * inch, width, 0.45 * inch, fill=1, stroke=0)

    canvas.setFillColor(colors.HexColor("#D58A2A"))
    canvas.rect(0.75 * inch, height - 0.55 * inch, 1.9 * inch, 0.06 * inch, fill=1, stroke=0)

    canvas.setFillColor(colors.HexColor("#17324D"))
    canvas.setFont("Helvetica", 8)
    footer = "os64 component review"
    canvas.drawString(doc.leftMargin, 0.45 * inch, footer)

    page_num = f"{canvas.getPageNumber()}"
    canvas.drawRightString(width - doc.rightMargin, 0.45 * inch, page_num)
    canvas.restoreState()


def build_summary_table(styles: StyleSheet1) -> Table:
    data = [["Component", "State", "Primary role"]]
    for component in COMPONENTS:
        data.append(
            [
                Paragraph(component.name, styles["TableBody"]),
                Paragraph(component.status, styles["TableBody"]),
                Paragraph(component.role, styles["TableBody"]),
            ]
        )

    table = Table(data, colWidths=[2.2 * inch, 1.0 * inch, 3.4 * inch], repeatRows=1)
    table.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#17324D")),
                ("TEXTCOLOR", (0, 0), (-1, 0), colors.whitesmoke),
                ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
                ("FONTSIZE", (0, 0), (-1, 0), 9),
                ("LEADING", (0, 0), (-1, 0), 11),
                ("BACKGROUND", (0, 1), (-1, -1), colors.HexColor("#FFFDFC")),
                ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.HexColor("#FFFDFC"), colors.HexColor("#F2EEE6")]),
                ("GRID", (0, 0), (-1, -1), 0.35, colors.HexColor("#C8BFAF")),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 6),
                ("RIGHTPADDING", (0, 0), (-1, -1), 6),
                ("TOPPADDING", (0, 0), (-1, -1), 4),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
            ]
        )
    )
    return table


def story() -> list:
    styles = build_styles()
    flow = []

    flow.append(Spacer(1, 1.1 * inch))
    flow.append(Paragraph("os64 Component Review", styles["DocTitle"]))
    flow.append(
        Paragraph(
            "A code-informed architecture summary for the hobby x86_64 kernel in "
            "/home/yogi/src/os64",
            styles["DocSubtitle"],
        )
    )
    flow.append(
        Paragraph(
            "This document summarizes how each major subsystem works today. The tone "
            "is intentionally calibrated for a hobby OS: the goal is to explain the "
            "shape of the system, call out what is already cohesive, and note where "
            "the implementation is clearly exploratory without treating that as a "
            "failure.",
            styles["BodySmall"],
        )
    )
    flow.append(
        Paragraph(
            "Review basis: source inspection across boot, memory, scheduler, SMP, "
            "storage, VFS, ELF, syscall, signal, and test paths.",
            styles["Meta"],
        )
    )
    flow.append(
        Paragraph(
            "Output: output/pdf/os64_component_review.pdf",
            styles["Meta"],
        )
    )
    flow.append(Spacer(1, 0.15 * inch))
    flow.append(Paragraph("System Map", styles["SectionTitle"]))
    flow.append(build_summary_table(styles))
    flow.append(PageBreak())

    flow.append(Paragraph("Overall Read", styles["SectionTitle"]))
    flow.append(
        Paragraph(
            "os64 already behaves like a real kernel, not just a boot demo. Its most "
            "mature path is the vertical slice from firmware discovery to storage to "
            "filesystem to task loading. The design favors explicit control flow, "
            "simple data structures, and heavy logging over deep abstraction, which "
            "is usually the right call in an educational system.",
            styles["BodySmall"],
        )
    )
    flow.append(
        Paragraph(
            "The project is also visibly in motion. Some areas, such as FAT-backed "
            "root mounting and scheduler-driven sleeping, are practical and well "
            "integrated. Others, such as richer syscall coverage, ext2 as a full "
            "runtime filesystem, and broader signal semantics, are clearly in the "
            "prototype phase. That balance feels appropriate for a hobby OS that is "
            "trying to learn from real subsystems instead of staying minimal forever.",
            styles["BodySmall"],
        )
    )

    for component in COMPONENTS:
        flow.append(Paragraph(component.name, styles["SectionTitle"]))
        flow.append(Paragraph(f"State: {component.status}", styles["Label"]))
        files_text = "Key files: " + ", ".join(component.files)
        flow.append(Paragraph(files_text, styles["Meta"]))
        flow.append(Paragraph(component.summary, styles["BodySmall"]))
        flow.append(Paragraph("Hobby OS review", styles["Label"]))
        flow.append(Paragraph(component.review, styles["BodySmall"]))

    flow.append(Paragraph("Closing Note", styles["SectionTitle"]))
    flow.append(
        Paragraph(
            "The strongest quality in this codebase is that the pieces connect. "
            "Boot data becomes allocator state, allocator state feeds paging, paging "
            "backs tasks, tasks interact with the scheduler, the scheduler sits on "
            "top of SMP and interrupt infrastructure, and storage culminates in a "
            "real mounted root and executable loading. That sort of end-to-end path "
            "is what turns a hobby kernel into a durable learning platform.",
            styles["BodySmall"],
        )
    )

    return flow


def build_pdf() -> None:
    OUTPUT_PDF.parent.mkdir(parents=True, exist_ok=True)
    PREVIEW_DIR.mkdir(parents=True, exist_ok=True)

    doc = SimpleDocTemplate(
        str(OUTPUT_PDF),
        pagesize=LETTER,
        leftMargin=0.75 * inch,
        rightMargin=0.75 * inch,
        topMargin=0.9 * inch,
        bottomMargin=0.75 * inch,
        title="os64 Component Review",
        author="OpenAI Codex",
        subject="Architecture review of the os64 hobby operating system",
    )
    doc.build(story(), onFirstPage=draw_page_chrome, onLaterPages=draw_page_chrome)


def render_previews() -> list[Path]:
    import pypdfium2 as pdfium

    for stale in PREVIEW_DIR.glob("os64_component_review_page_*.png"):
        stale.unlink()

    pdf = pdfium.PdfDocument(str(OUTPUT_PDF))
    output_paths: list[Path] = []
    for idx in range(len(pdf)):
        page = pdf[idx]
        image = page.render(scale=1.6).to_pil()
        path = PREVIEW_DIR / f"os64_component_review_page_{idx + 1:02d}.png"
        image.save(path)
        output_paths.append(path)
    return output_paths


def validate_pdf() -> tuple[int, list[str]]:
    from pypdf import PdfReader

    reader = PdfReader(str(OUTPUT_PDF))
    text = "\n".join((page.extract_text() or "") for page in reader.pages)
    required = [
        "os64 Component Review",
        "Memory Management",
        "Tasks And Threads",
        "VFS And Filesystems",
        "ELF Loading And Demand Paging",
    ]
    missing = [item for item in required if item not in text]
    return len(reader.pages), missing


def main() -> int:
    build_pdf()
    preview_paths = render_previews()
    page_count, missing = validate_pdf()

    print(f"PDF: {OUTPUT_PDF}")
    print(f"Pages: {page_count}")
    print("Preview images:")
    for path in preview_paths:
        print(path)

    if missing:
        print("Missing expected text markers:")
        for item in missing:
            print(f"- {item}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
