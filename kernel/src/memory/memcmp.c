#include "strings.h"

#include <stddef.h>  // For size_t

///@brief Compare two blocks of memory byte by byte.
 /// This function compares the first `num` bytes of the memory areas pointed to by
 /// `ptr1` and `ptr2`. It stops as soon as a mismatch is found or after `num` bytes
 /// are compared. The comparison is performed as unsigned byte values.
 ///
 /// @param ptr1 Pointer to the first block of memory.
 /// @param ptr2 Pointer to the second block of memory.
 /// @param num  Number of bytes to compare.
 ///
 /// @return An integer less than, equal to, or greater than zero if the first
 ///         differing byte in `ptr1` is found to be less than, equal to, or greater
 ///         than the corresponding byte in `ptr2`.
 ///         Returns 0 if the memory blocks are identical for the first `num` bytes.
 int memcmp(const void *ptr1, const void *ptr2, size_t num) {
    const unsigned char *a = (const unsigned char *)ptr1;
    const unsigned char *b = (const unsigned char *)ptr2;

    for (size_t i = 0; i < num; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];  // Return the difference between the first mismatched bytes
        }
    }

    return 0;  // Memory blocks are equal
}
