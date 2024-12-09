#include "strings.h"

void strtrim(char *input)
{
   char *dst = input, *src = input;
   char *end;

   // Skip whitespace at front...
   //
   while (*src==' ')
   {
      ++src;
   }

   // Trim at end...
   //
   end = src + strlen(src) - 1;
   while (end > src && *end==' ')
   {
      *end-- = 0;
   }

   // Move if needed.
   //
   if (src != dst)
   {
      while ((*dst++ = *src++));
   }
}
