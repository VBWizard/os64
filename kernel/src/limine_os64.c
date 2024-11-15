#include <stdint.h>
#include <stddef.h>
#include "limine_os64.h"

 __attribute__((used, section(".limine_requests")))
volatile struct limine_framebuffer_request framebuffer_request = {
     .id = LIMINE_FRAMEBUFFER_REQUEST,
     .revision = 0
 };
 
 __attribute__((used, section(".limine_requests")))
volatile struct limine_memmap_request memmap_request = {
     .id = LIMINE_MEMMAP_REQUEST,
     .revision = 0
 };

 __attribute__((used, section(".limine_requests")))
volatile struct limine_kernel_address_request kernel_address_request = {
     .id = LIMINE_KERNEL_ADDRESS_REQUEST,
     .revision = 0,
 };

 __attribute__((used, section(".limine_requests")))
volatile struct limine_hhdm_request hhmd_request = {
     .id = LIMINE_HHDM_REQUEST,
     .revision = 0,
 };

 __attribute__((used, section(".limine_requests")))
volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

 // GCC and Clang reserve the right to generate calls to the following
// 4 functions even if they are not directly called.
// Implement them as the C specification mandates.
// DO NOT remove or rename these functions, or stuff will eventually break!
// They CAN be moved to a different .c file.
void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++) {
        pdest[i] = psrc[i];
    }

    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    if (src > dest) {
        for (size_t i = 0; i < n; i++) {
            pdest[i] = psrc[i];
        }
    } else if (src < dest) {
        for (size_t i = n; i > 0; i--) {
            pdest[i-1] = psrc[i-1];
        }
    }

    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
}

