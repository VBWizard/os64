// Include the license in the same retained section as the unchanged firmware:
// a kernel-only update must carry Intel's redistribution notice with the blob.
.section .rodata.bt_firmware,"a",@progbits
.global bt_ax210_sfi, bt_ax210_sfi_end, bt_ax210_ddc, bt_ax210_ddc_end
bt_ax210_sfi:
.incbin "../firmware/intel/ibt-0041-0041.sfi"
bt_ax210_sfi_end:
bt_ax210_ddc:
.incbin "../firmware/intel/ibt-0041-0041.ddc"
bt_ax210_ddc_end:
.incbin "../firmware/intel/LICENCE.ibt_firmware"
.byte 0
