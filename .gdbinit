# GDB initialization for os64 kernel debugging (CLI gdb).
# VS Code does NOT read this file — its equivalent lives in
# .vscode/launch.json setupCommands; the shared logic is in
# utility/os64_symbols.gdb so both stay in sync.
set architecture i386:x86-64
target remote localhost:1234

# Load kernel symbols
symbol-file kernel/bin/os64_kernel

# Shared settings + per-app symbol autoloader (see comments in the file)
source utility/os64_symbols.gdb

# ── Breakpoints in user programs: use hbreak ─────────────────────────────────
# Software breakpoints (int3) in app code misbehave twice over: the target
# page is demand-paged (not yet mapped when the breakpoint is inserted), and
# insertion happens under whatever CR3 is current (usually not the app's).
# Hardware breakpoints match the linear address in ANY address space and need
# no memory write:
#     hbreak syscall_smoke.c:_start     (goes pending until symbols autoload)
#     hbreak *0x400050
# Note x86 has only 4 hardware breakpoint slots.

# Useful breakpoints (commented out - uncomment as needed)
# hbreak *0x400000
# break handle_page_fault
# break task_exit
# break vma_resolve_backing_page

# Display settings
set disassembly-flavor intel
set pagination off

# Show where we are
info registers rip rsp rbp
