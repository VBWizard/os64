#include "strings.h"
#include "stddef.h"
/**
 * is_whitespace - Determines if a character is a whitespace character.
 * @c: The character to check.
 *
 * Returns 1 if the character is a whitespace character, 0 otherwise.
 */
int is_whitespace(char c) {
    return (c == ' ')  ||
           (c == '\t') ||
           (c == '\n') ||
           (c == '\r') ||
           (c == '\v') ||
           (c == '\f');
}

/**
 * strtrim - Trims leading and trailing whitespace from a string in place.
 * @str: The input string to be trimmed. Must be mutable.
 *
 * Returns the same pointer to the trimmed string.
 * If str is NULL, returns NULL.
 */
char *strtrim(char *str) {
    if (str == NULL) {
        return NULL;
    }

    size_t len = strlen(str);
    size_t start = 0;
    size_t end = len;

    // Find the index of the first non-whitespace character
    while (start < len && is_whitespace(str[start])) {
        start++;
    }

    // If the string is all whitespace
    if (start == len) {
        str[0] = '\0';
        return str;
    }

    // Find the index of the last non-whitespace character
    end = len - 1;
    while (end > start && is_whitespace(str[end])) {
        end--;
    }

    // Calculate the length of the trimmed string
    size_t trimmed_length = end - start + 1;

    // If there is leading whitespace, shift the string left
    if (start > 0) {
        size_t i;
        for (i = 0; i < trimmed_length; i++) {
            str[i] = str[start + i];
        }
    }

    // Null-terminate the string after the last non-whitespace character
    str[trimmed_length] = '\0';

    return str;
}