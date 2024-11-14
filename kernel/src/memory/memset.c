#include <stdint.h>
#include <stddef.h>

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

void *memset(void *d1, int val, size_t len) {
    uint8_t *d = d1;
    uint64_t val64 = (uint64_t)(val & 0xFF);
    val64 |= val64 << 8;
    val64 |= val64 << 16;
    val64 |= val64 << 32;

    // Handle unaligned start
    while (((uintptr_t)d & 7) && len) {
        *d++ = (uint8_t)val;
        len--;
    }

    // Main loop: set memory 8 bytes at a time, with page boundary checks once per page
    while (len >= 8) {
        // Calculate the remaining bytes in the current page
        size_t bytes_to_page_end = PAGE_SIZE - ((uintptr_t)d & (PAGE_SIZE - 1));

        // Copy as many 8-byte chunks as possible within the page
        size_t chunk_len = (len < bytes_to_page_end) ? len : bytes_to_page_end;
        uint64_t *d64 = (uint64_t *)d;

        // Copy in 8-byte chunks until we reach the end of the page or the specified length
        while (chunk_len >= 8) {
            *d64++ = val64;
            chunk_len -= 8;
            len -= 8;
        }

        // Move the pointer to the next position after the chunks
        d = (uint8_t *)d64;
    }

    // Handle remaining bytes one at a time
    while (len--) {
        *d++ = (uint8_t)val;
    }

    return d1;
}
