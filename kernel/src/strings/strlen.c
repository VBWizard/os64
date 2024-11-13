#include <strlen.h>

size_t strlen(const char* str) {
          size_t ret = 0;
        while ( str[ret] != 0 )
                ret++;
        return ret;
}

size_t strnlen(const char* str, int maxLen) {
    size_t ret = 0;
    int len = 0;

          while ( str[ret] != 0 )
        {
                ret++;
                if (++len==maxLen)
                    break;
        }
        return ret;
}
