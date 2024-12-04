#include "math.h"

#include <stdint.h>

uint8_t log2(uint32_t x) {
    uint8_t result = 0;

    // Find the most significant bit (MSB)
    while (x >>= 1) {
        result++;
    }

    return result;
}

uint8_t log2_64(uint64_t x) {
    uint8_t result = 0;

    // Find the most significant bit (MSB)
    while (x >>= 1) {
        result++;
    }

    return result;
}