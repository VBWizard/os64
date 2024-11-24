char *strcpy(char *s1, const char *s2) 
{
    char *s = s1;
    while ((*s++ = *s2++) != 0)
        ;
    return s1;
}

#include <stddef.h>

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;

    // Copy characters from src to dest until n characters are copied
    // or until the end of src is reached
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];

    // If the end of src is reached before n characters,
    // pad the rest of dest with null characters
    for (; i < n; i++)
        dest[i] = '\0';

    return dest;
}
