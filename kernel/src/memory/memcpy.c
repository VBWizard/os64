#include "memory/memcpy.h"

void *memcpy(void *dest1, const void *src1, size_t len) {
    uint8_t *dest = (uint8_t *)dest1;
    const uint8_t *src = (const uint8_t *)src1;

    // Handle unaligned start
    while (((uintptr_t)dest & 7) && len) {
        *dest++ = *src++;
        len--;
    }

    // Main loop: copy memory 8 bytes at a time, with page boundary checks once per page
    while (len >= 8) {
        // Calculate the remaining bytes in the current page
        size_t bytes_to_page_end = PAGE_SIZE - ((uintptr_t)dest & (PAGE_SIZE - 1));

        // Copy as many 8-byte chunks as possible within the page
        size_t chunk_len = (len < bytes_to_page_end) ? len : bytes_to_page_end;
        uint64_t *dest64 = (uint64_t *)dest;
        const uint64_t *src64 = (const uint64_t *)src;

        // Copy in 8-byte chunks until we reach the end of the page or the specified length
        while (chunk_len >= 8) {
            *dest64++ = *src64++;
            chunk_len -= 8;
            len -= 8;
        }

        // Move the pointers to the next position after the chunks
        dest = (uint8_t *)dest64;
        src = (const uint8_t *)src64;
    }

    // Handle remaining bytes one at a time
    while (len--) {
        *dest++ = *src++;
    }

    return dest1;
}
