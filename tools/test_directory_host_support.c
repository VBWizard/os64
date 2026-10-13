#include "test_directory_host_support.h"
#include "os64/date.h"
#include "os64/mem.h"
#include "os64/str.h"
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdint.h>

const os64_dirent_t *test_entries;
size_t test_entry_count;
int test_read_error_at=-1,test_close_error,test_open_error,test_alloc_fail;
const char *test_mounts;
const char *test_home="/home/chris";
static size_t entry_cursor,mount_cursor;
void test_reset_io(void)
{ entry_cursor=mount_cursor=0; test_read_error_at=-1; test_close_error=test_open_error=test_alloc_fail=0; }
void *os64_malloc(size_t n) { if (test_alloc_fail && !--test_alloc_fail) return NULL; return malloc(n); }
void *os64_calloc(size_t n,size_t size) { if (test_alloc_fail && !--test_alloc_fail) return NULL; return calloc(n,size); }
void os64_free(void *p) { free(p); }
size_t os64_malloc_size(const void *p) { return p?malloc_usable_size((void *)p):0; }
void *os64_memcpy(void *d,const void *s,size_t n) { return memcpy(d,s,n); }
void *os64_memmove(void *d,const void *s,size_t n) { return memmove(d,s,n); }
void *os64_memset(void *d,int c,size_t n) { return memset(d,c,n); }
int os64_memcmp(const void *a,const void *b,size_t n) { return memcmp(a,b,n); }
size_t os64_strlen(const char *s) { return strlen(s); }
bool os64_streq(const char *a,const char *b) { return strcmp(a,b)==0; }
const char *os64_getenv(const char *name) { return strcmp(name,"HOME")==0?test_home:NULL; }
int64_t os64_opendir(const char *path) { (void)path; entry_cursor=0; return test_open_error?-9:11; }
int64_t os64_readdir(int32_t h,os64_dirent_t *out)
{ (void)h; if (test_read_error_at>=0 && entry_cursor==(size_t)test_read_error_at) return -7;
  if (entry_cursor==test_entry_count) return 0;
  *out=test_entries[entry_cursor++]; return 1; }
int64_t os64_close(int32_t h) { (void)h; return test_close_error?-8:0; }
int64_t os64_open(const char *path,const char *mode)
{ (void)mode; if (strcmp(path,"/sys/mounts") || !test_mounts) return -1; mount_cursor=0; return 12; }
int64_t os64_read(int32_t h,void *buf,size_t cap)
{ (void)h; size_t n=test_mounts?strlen(test_mounts):0; if (mount_cursor==n) return 0;
  size_t take=n-mount_cursor<cap?n-mount_cursor:cap; memcpy(buf,test_mounts+mount_cursor,take); mount_cursor+=take; return (int64_t)take; }
void os64_unescape_field(char *s)
{ char *w=s; for (char *r=s;*r;++r) { if (r[0]=='\\' && r[1]=='x' && r[2] && r[3]) {
      char a=r[2],b=r[3]; int x=(a>='0'&&a<='9'?a-'0':a>='A'&&a<='F'?a-'A'+10:-1);
      int y=(b>='0'&&b<='9'?b-'0':b>='A'&&b<='F'?b-'A'+10:-1);
      if (x>=0&&y>=0) { *w++=(char)(x*16+y);r+=3;continue; } }
    *w++=*r; } *w=0; }
int64_t os64_localtime(int64_t epoch,os64_date_t *out)
{ if (epoch<0) return -1; memset(out,0,sizeof(*out)); out->year=2026;out->month=10;out->day=13;return 0; }
size_t os64_strftime(char *buffer,size_t cap,const char *fmt,const os64_date_t *date)
{ (void)fmt;(void)date; const char *s="2026-10-13 00:00";size_t n=strlen(s);
  if (n+1>cap) return 0;
  memcpy(buffer,s,n+1);return n; }
