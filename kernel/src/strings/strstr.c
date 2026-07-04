#include "strstr.h"

char* strstr(const char* string, const char* substring)
{
    register const char *a, *b;

    /* First scan quickly through the two strings looking for a
     * single-character match.  When it's found, then compare the
     * rest of the substring.
     */

    b = substring;
    if (*b == 0) 
    {
        return (char *)string;
    }
    for ( ; *string != 0; string += 1) 
    {
        if (*string != *b) 
        {
            continue;
        }
        a = string;
        while (1) 
        {
            if (*b == 0) 
            {
                return (char *)string;
            }
            if (*a++ != *b++) 
            {
                break;
            }
        }
        b = substring;
    }
    return (char *)0;
}

char* strnstr(const char* string, const char* substring, int length)
{
    if (length <= 0) {
        return (char *)0;
    }

    if (*substring == 0) {
        return (char *)string;
    }

    for (int i = 0; i < length && string[i] != 0; i++) {
        int j = 0;
        while (i + j < length && substring[j] != 0 && string[i + j] == substring[j]) {
            j++;
        }
        if (substring[j] == 0) {
            return (char *)(string + i);
        }
    }

    return (char *)0;
}
