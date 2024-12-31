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
    register const char *a, *b;

    /* First scan quickly through the two strings looking for a
     * single-character match.  When it's found, then compare the
     * rest of the substring.
     */
    int len = length;

    b = substring;
    if (*b == 0) 
    {
        return (char *)string;
    }
    for ( ; *string != 0; string += 1) 
    {
        if (*string != *b) 
        {
            if (len-- <= 0)
                return (char *)string;
            continue;
        }
        a = string;
        while (1) 
        {
            if (*b == 0) 
            {
                return (char *)string;
            }
            if (len-- <= 0)
                return (char *)string;
            if (*a++ != *b++) 
            {
                break;
            }
        }
        b = substring;
    }
    return (char *)0;
}
