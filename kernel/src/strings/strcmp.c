#include "strings/strcmp.h"

/// @brief 
/// @param p1 
/// @param p2 
/// @return 
int strcmp (const char *p1, const char *p2)
{
  register const unsigned char *s1 = (const unsigned char *) p1;
  register const unsigned char *s2 = (const unsigned char *) p2;
  unsigned char c1, c2;

  do
    {
      c1 = (unsigned char) *s1++;
      c2 = (unsigned char) *s2++;
      if (c1 == '\0')
	return c1 - c2;
    }
  while (c1 == c2);

  return c1 - c2;
}


/**
 * @brief Compares the NULL terminated string pointed to by `p1` with the NULL terminated string 
 * pointed to by `p2` lexicographically. The comparison stops after `n` characters, 
 * or when a null character is encountered in either string, whichever comes first.
 * 
 * @param s1 Pointer to the first null-terminated string.
 * @param s2 Pointer to the second null-terminated string.
 * @param n Maximum number of characters to compare.
 * @return int Returns an integer less than, equal to, or greater than zero if
 *         `p1` is found, respectively, to be less than, to match, or be greater
 *         than `p2` up to the first `n` characters.
 */
int strncmp(const char *s1, const char *s2, size_t n)
{
    for ( ; n > 0; s1++, s2++, --n)
	if (*s1 != *s2)
	    return ((*(unsigned char *)s1 < *(unsigned char *)s2) ? -1 : +1);
	else if (*s1 == '\0')
	    return 0;
    return 0;
}
