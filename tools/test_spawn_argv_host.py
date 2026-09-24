#!/usr/bin/env python3
"""Compile spawn's actual argument measure and copy passes and race them.

user_string_length, measure_user_argv and marshal_user_argv come out of
kernel/src/syscall.c unchanged. "User memory" is host memory, with a real
PROT_NONE page standing in for an unreadable one, so a pass that reads past
what it vetted dies under the sanitizer instead of passing. The stubbed copy
can change user memory at chosen moments, the way a second thread of the
spawning program can: between the measure and the copy, and between the
copy pass's own re-measure and its read.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'kernel/src/syscall.c').read_text()


def function(signature):
    match = re.search(re.escape(signature) + r'\([^;]+?\)\s*\{', source)
    assert match, signature
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


enum = re.search(r'typedef enum\s*\{[^}]+\}\s*argv_measure_t;', source)
assert enum, 'argv_measure_t'

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "os64/syscall_numbers.h"

#define PAGE_SIZE 4096
#define SPAWN_MAX_ARGS 512
#define TASK_ARGV_MAX_BYTES 0x100000
static const uintptr_t kHHDMOffset = (uintptr_t)0xffff800000000000ull;

// One real hole: a page nobody may read.
static char *hole;

static bool user_range_accessible(const void *p, size_t len, bool for_write)
{
    (void)for_write;
    uintptr_t a = (uintptr_t)p, h = (uintptr_t)hole;
    return a + len <= h || a >= h + PAGE_SIZE;
}
static uint64_t user_cr3_window_open(bool *switched) { *switched = false; return 0; }
static void user_cr3_window_close(uint64_t cr3, bool switched) { (void)cr3; (void)switched; }

// The racing sibling: runs just before the copy numbered `race_at`.
static int copies, race_at = -1;
static void (*race)(void);
static bool copy_user_buffer(const void *src, void *dst, size_t len)
{
    if (++copies == race_at && race)
        race();
    if (!user_range_accessible(src, len, false))
        return false;
    memcpy(dst, src, len);
    return true;
}
''' + enum.group(0) + function('static int user_string_length') \
    + function('static argv_measure_t measure_user_argv') \
    + function('static int marshal_user_argv') + r'''

static char *pattern(size_t len)
{
    char *s = malloc(len + 1);
    for (size_t i = 0; i < len; i++)
        s[i] = (char)('a' + i % 26);
    s[len] = '\0';
    return s;
}

static argv_measure_t measure(char **argv, int *argc, size_t *bytes)
{
    copies = 0;
    return measure_user_argv(argv, argc, bytes);
}

// Measure, then copy into exactly what the measure paid for — with the
// sibling armed for the copy pass only, before its read number `at`.
static int marshal(char **argv, char ***kargv_out, char **strbuf_out,
                   void (*sibling)(void), int at)
{
    int argc; size_t bytes;
    if (measure(argv, &argc, &bytes) != ARGV_FITS)
        return -100;
    *kargv_out = calloc((size_t)argc + 1, sizeof(char *));
    *strbuf_out = malloc(bytes ? bytes : 1);
    copies = 0;
    race = sibling;
    race_at = at;
    int got = marshal_user_argv(argv, *kargv_out, *strbuf_out, bytes, argc);
    race = NULL;
    race_at = -1;
    return got;
}

static char **g_argv;
static char *g_short, *g_long;
static void grow_first(void)  { g_argv[1] = g_long; }
static void grow_first_shrink_second(void) { g_argv[1] = g_long; g_argv[2] = ""; }
static void shrink_first(void) { g_argv[1] = g_short; }
static void add_entry(void)   { g_argv[3] = "extra"; g_argv[4] = NULL; }
static void lengthen_in_place(void) { g_argv[1][3] = 'Z'; }   // the NUL moves on

int main(void)
{
    hole = mmap(NULL, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(hole != MAP_FAILED);
    hole += PAGE_SIZE;
    assert(mprotect(hole, PAGE_SIZE, PROT_NONE) == 0);

    int argc; size_t bytes;
    assert(measure(NULL, &argc, &bytes) == ARGV_FITS && argc == 0 && bytes == 0);
    char *two[] = { "a", "bc", NULL };
    assert(measure(two, &argc, &bytes) == ARGV_FITS && argc == 2 && bytes == 5);

    // The per-argument cap counts the terminator.
    char *at_cap = pattern(OS64_SPAWN_ARG_MAX - 1), *over = pattern(OS64_SPAWN_ARG_MAX);
    char *one[] = { at_cap, NULL };
    assert(measure(one, &argc, &bytes) == ARGV_FITS && bytes == (size_t)OS64_SPAWN_ARG_MAX);
    one[0] = over;
    assert(measure(one, &argc, &bytes) == ARGV_TOO_LONG);

    // 512 arguments fit; a 513th does not.
    char *many[515];
    for (int i = 0; i < 514; i++) many[i] = "x";
    many[512] = NULL;
    assert(measure(many, &argc, &bytes) == ARGV_FITS && argc == 512);
    many[512] = "x"; many[513] = NULL;
    assert(measure(many, &argc, &bytes) == ARGV_TOO_LONG);

    // The block, exactly: eight strings whose pointers and bytes fill it to
    // the last byte fit, and one more byte does not.
    size_t each = (TASK_ARGV_MAX_BYTES - 9 * sizeof(char *)) / 8 - 1;
    assert((9 * sizeof(char *)) + 8 * (each + 1) == TASK_ARGV_MAX_BYTES);
    char *fill = pattern(each), *fill1 = pattern(each + 1);
    char *eight[9];
    for (int i = 0; i < 8; i++) eight[i] = fill;
    eight[8] = NULL;
    assert(measure(eight, &argc, &bytes) == ARGV_FITS && argc == 8);
    eight[7] = fill1;
    assert(measure(eight, &argc, &bytes) == ARGV_TOO_LONG);

    // A string that ends on the last byte before the hole is read without
    // touching it; one that runs into the hole is unreadable, not too long.
    char *edge = hole - 4;
    memcpy(edge, "abc", 4);
    char *edge_argv[] = { edge, NULL };
    assert(measure(edge_argv, &argc, &bytes) == ARGV_FITS && bytes == 4);
    memcpy(edge, "abcd", 4);
    assert(measure(edge_argv, &argc, &bytes) == ARGV_UNREADABLE);
    assert(measure((char **)(hole + 8), &argc, &bytes) == ARGV_UNREADABLE);

    // The copy pass: what arrives is what was measured.
    char **kargv, *strbuf;
    char *plain[] = { "prog", "hello", "", NULL };
    assert(marshal(plain, &kargv, &strbuf, NULL, -1) == 3);
    assert(!strcmp(kargv[0], "prog") && !strcmp(kargv[1], "hello") && !strcmp(kargv[2], "") && !kargv[3]);

    // The copy pass's reads, in order: argv[0]'s pointer, argv[0]'s bytes,
    // argv[1]'s pointer, argv[1]'s bytes. The sibling runs before one of them.

    // A string that grows between the passes past the room paid for.
    g_long = pattern(4000);
    char *racing[] = { "prog", "abc", NULL, NULL, NULL };
    g_argv = racing;
    assert(marshal(racing, &kargv, &strbuf, grow_first, 2) == -1);

    // One grows past the per-argument cap while another shrinks to pay for
    // it, so the room paid for still holds both: the room is not the only
    // promise, and the cap refuses it.
    char *swap[] = { "prog", pattern(OS64_SPAWN_ARG_MAX - 100), pattern(OS64_SPAWN_ARG_MAX - 100), NULL, NULL };
    g_argv = swap;
    g_long = pattern(OS64_SPAWN_ARG_MAX + 1000);
    assert(marshal(swap, &kargv, &strbuf, grow_first_shrink_second, 2) == -1);

    // A string that shrinks arrives shorter — what the program holds now.
    char *shrinking[] = { "prog", pattern(40), NULL, NULL, NULL };
    g_argv = shrinking; g_short = "ok";
    assert(marshal(shrinking, &kargv, &strbuf, shrink_first, 2) == 2 && !strcmp(kargv[1], "ok"));

    // An entry added between the passes is not copied past what was paid for.
    char *adding[] = { "prog", "a", "b", NULL, NULL };
    g_argv = adding;
    assert(marshal(adding, &kargv, &strbuf, add_entry, 1) == 3 && kargv[3] == NULL);

    // The terminator moves between the copy pass's own measure and its read.
    char *moving[] = { "prog", strdup("abc"), NULL };
    g_argv = moving;
    assert(marshal(moving, &kargv, &strbuf, lengthen_in_place, 4) == -1);

    puts("spawn argv: caps exact at the terminator, the 512th argument and the block's last byte; "
         "the hole is never read; growth, cap-dodging and a moved terminator between passes refused, "
         "shrinking and added entries bounded PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='spawn-argv-') as directory:
    work = Path(directory)
    path = work / 'spawn_argv.c'
    path.write_text(program)
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I' + str(root / 'abi/include'), str(path), '-o', str(work / 'spawn_argv')],
                   check=True)
    # The scenarios keep what they build for the whole run; leaks are not the question.
    result = subprocess.run([str(work / 'spawn_argv')],
                            env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
    assert result.returncode == 0, result.returncode
