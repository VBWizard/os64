#!/bin/bash
objdump -S -l kernel/bin/os64_kernel -M intel-mnemonic --architecture=x86_64 > os64_kernel_debug.asm
vi os64_kernel_debug.asm

