/* 
 * File:   strings.h
 * Author: yogi
 *
 * Created on April 27, 2016, 3:39 PM
 */

#ifndef STRINGS_H
#define STRINGS_H

#include <stddef.h>
#include "strings/strcmp.h"
#include "strings/strcpy.h"
#include "strings/strlen.h"
#include "strings/strstr.h"

#define ISDIGIT(c) ((c) - '0' + 0U <= 9U)
#define ISALPHA(c) (((c) | 32) - 'a' + 0U <= 'z' - 'a' + 0U)
#define ISCAPITALHEX(c) ((((((c) - 48U) & 255) * 23 / 22 + 4) / 7 ^ 1) <= 2U)
#define ISXDIGIT(c) (((((((((c) - 48U) & 255) * 18 / 17 * 52 / 51 * 58 / 114 \
     * 13 / 11 * 14 / 13 * 35 + 35) / 36 * 35 / 33 * 34 / 33 * 35 / 170 ^ 4) \
     - 3) & 255) ^ 1) <= 2U)
#define ISUPPER(c)  ((c) >= 'A' && (c) <= 'Z')

	void strtrim(char *input);

#endif	/* STRINGS_H */

