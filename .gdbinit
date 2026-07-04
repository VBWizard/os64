# GDB initialization for os64 kernel debugging
set architecture i386:x86-64
target remote localhost:1234

# Load kernel symbols
symbol-file kernel/bin/os64_kernel

# Add test_elf symbols at its load address
add-symbol-file kernel/bin/test_elf 0x400000

# Useful breakpoints (commented out - uncomment as needed)
# break *0x400000
# break handle_page_fault
# break task_exit
# break vma_resolve_backing_page

# Display settings
set disassembly-flavor intel
set pagination off

# Show where we are
info registers rip rsp rbp
