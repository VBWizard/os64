#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "jpeg/jpeg.h"
static _Thread_local size_t live, calls, fail_at;
void *os64_malloc(size_t n) { if (++calls == fail_at) return NULL; void *p=malloc(n); if(p)live++; return p; }
void os64_free(void *p) { if(p){assert(live);live--;free(p);} }
void *os64_memcpy(void *d,const void *s,size_t n){return memcpy(d,s,n);}
void *os64_memset(void *d,int c,size_t n){return memset(d,c,n);}
static unsigned char *load(const char *name,size_t *n)
{FILE *f=fopen(name,"rb");assert(f);assert(!fseek(f,0,SEEK_END));*n=ftell(f);rewind(f);unsigned char *p=malloc(*n);assert(p);assert(fread(p,1,*n,f)==*n);fclose(f);return p;}
typedef struct { const unsigned char *p; size_t n; } parallel_input;
static void *parallel_decode(void *context)
{
 parallel_input *in=context;
 for(unsigned i=0;i<20;i++) {
  os64_jpeg_image_t image;
  assert(os64_jpeg_decode(in->p,in->n,0,0,&image)==OS64_JPEG_OK);
  os64_jpeg_free(&image);
  assert(os64_jpeg_decode(in->p,in->n-1,0,0,&image)==OS64_JPEG_MALFORMED);
  assert(!image.pixels && !live);
 }
 return NULL;
}
int main(int argc,char **argv)
{
 assert(argc==3);size_t n,en;unsigned char *p=load(argv[1],&n),*e=load(argv[2],&en);
 os64_jpeg_image_t image;os64_jpeg_status_t s=os64_jpeg_decode(p,n,0,0,&image);
 if(en==1){assert(s==e[0]);assert(!image.pixels && !image.width && !image.height);assert(!live);free(e);free(p);return 0;}
 if(s){fprintf(stderr,"%s: %s\n",argv[1],os64_jpeg_status_name(s));return 1;}
 uint32_t w,h;memcpy(&w,e,4);memcpy(&h,e+4,4);assert(image.width==w && image.height==h && en==8+(size_t)w*h*4);
 for(size_t i=0;i<(size_t)w*h*4;i++) {int d=((unsigned char*)image.pixels)[i]-e[8+i];if(d < -2 || d > 2){fprintf(stderr,"%s: byte %zu got %u expected %u\n",argv[1],i,((unsigned char*)image.pixels)[i],e[8+i]);abort();}}
 os64_jpeg_free(&image);os64_jpeg_free(&image);assert(!live);
 assert(os64_jpeg_decode(p,n,(uint64_t)w*h-1,0,&image)==OS64_JPEG_LIMIT);assert(!live);
 assert(os64_jpeg_decode(p,n,0,1024,&image)==OS64_JPEG_LIMIT);assert(!live);
 for(size_t end=0;end<n;end++){
  s=os64_jpeg_decode(p,end,0,0,&image);assert(s!=OS64_JPEG_OK);assert(!image.pixels && !live);
 }
 for(fail_at=1;fail_at<100;fail_at++){
  calls=0;s=os64_jpeg_decode(p,n,0,0,&image);
  if(s==OS64_JPEG_OK){os64_jpeg_free(&image);assert(!live);break;}
  assert(s==OS64_JPEG_NO_MEMORY);assert(!image.pixels && !live);
 }
 assert(fail_at<100);fail_at=0;
 unsigned char *mutated=malloc(n);assert(mutated);
 uint32_t seed=0x64abc;
 for(unsigned round=0;round<300;round++) {
  memcpy(mutated,p,n);seed=seed*1664525u+1013904223u;
  size_t at=seed%n;mutated[at]^=(unsigned char)(1u<<((seed>>24)&7));
  s=os64_jpeg_decode(mutated,n,4096,4u*1024u*1024u,&image);
  if(s==OS64_JPEG_OK)os64_jpeg_free(&image);
  else assert(!image.pixels && !image.width && !image.height);
  assert(!live);
 }
 free(mutated);
 parallel_input input={p,n};pthread_t first,second;
 assert(!pthread_create(&first,NULL,parallel_decode,&input));
 assert(!pthread_create(&second,NULL,parallel_decode,&input));
 assert(!pthread_join(first,NULL));assert(!pthread_join(second,NULL));
 free(e);free(p);return 0;
}
