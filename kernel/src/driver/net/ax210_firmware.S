// Keep Intel's binary redistribution notice in the same retained section.
.section .rodata.ax210_firmware,"a",@progbits
.global ax210_ucode_start, ax210_ucode_end, ax210_pnvm_start, ax210_pnvm_end
ax210_ucode_start:
.incbin "../firmware/intel/ax210-wifi/iwlwifi-ty-a0-gf-a0-77.ucode"
ax210_ucode_end:
ax210_pnvm_start:
.incbin "../firmware/intel/ax210-wifi/iwlwifi-ty-a0-gf-a0.pnvm"
ax210_pnvm_end:
.incbin "../firmware/intel/ax210-wifi/LICENCE.iwlwifi_firmware"
.section .note.GNU-stack,"",@progbits
