/* THE REPLAY RUNNER'S DOOR TO THE HOST'S FILES (tools/test_yonder_replay_host.c
 * includes this file): every file and directory the runner reads or writes
 * goes through these, and each answers one of three, which its caller must
 * meet: the thing was done; it is ABSENT, which is an answer only where the
 * caller says nothing there is legitimate; or a FAULT, with why, which the
 * caller makes an UNJUDGED page or a refused run, never an empty answer.
 * tools/test_yonder_replay_host.sh fails the self-test when the runner calls
 * the host's file functions anywhere but here. */
#include <dirent.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef enum { RP_FS_OK, RP_FS_ABSENT, RP_FS_FAULT } RpFs;
/* Why a call faulted: the path and what the host said. */
typedef struct { char why[1024]; } RpFault;

static RpFs rp_fs_fault(RpFault *f, const char *path, const char *what, int err) {
    snprintf(f->why,sizeof(f->why),"%s %s: %s",path,what,strerror(err));
    return RP_FS_FAULT;
}

/* A whole file into `*bytes` (malloc'd, NUL-terminated past `*len`). */
static RpFs rp_fs_read(const char *path, char **bytes, size_t *len, RpFault *f) {
    *bytes=NULL; if(len!=NULL) *len=0;
    FILE *file=fopen(path,"rb");
    if(file==NULL) return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,path,"cannot be opened",errno);
    char *data=NULL; size_t n=0, cap=0;
    for(;;) {
        if(n+65536+1>cap) {
            cap=cap ? cap*2 : 131072;
            char *grown=realloc(data,cap);
            if(grown==NULL) { free(data); fclose(file); return rp_fs_fault(f,path,"cannot be held",ENOMEM); }
            data=grown;
        }
        size_t got=fread(data+n,1,65536,file);
        n+=got;
        if(got==0) break;
    }
    int err=ferror(file) ? (errno ? errno : EIO) : 0;
    fclose(file);
    if(err) { free(data); return rp_fs_fault(f,path,"cannot be read",err); }
    data[n]='\0';
    *bytes=data; if(len!=NULL) *len=n;
    return RP_FS_OK;
}

/* `len` bytes written as the whole of `path`. */
static RpFs rp_fs_write(const char *path, const void *bytes, size_t len, RpFault *f) {
    FILE *file=fopen(path,"wb");
    if(file==NULL) return rp_fs_fault(f,path,"cannot be made",errno);
    bool ok=fwrite(bytes,1,len,file)==len;
    int err=errno;
    if(fclose(file)!=0 && ok) { ok=false; err=errno; }
    return ok ? RP_FS_OK : rp_fs_fault(f,path,"cannot be written",err ? err : EIO);
}

/* What is at `path`: a directory (`*dir` true) or anything else. */
static RpFs rp_fs_kind(const char *path, bool *dir, RpFault *f) {
    struct stat st;
    if(stat(path,&st)!=0) return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,path,"cannot be looked at",errno);
    *dir=S_ISDIR(st.st_mode);
    return RP_FS_OK;
}

/* A directory made, or one already there. */
static RpFs rp_fs_mkdir(const char *path, RpFault *f) {
    if(mkdir(path,0777)==0) return RP_FS_OK;
    int err=errno;
    bool dir=false;
    if(err==EEXIST && rp_fs_kind(path,&dir,f)==RP_FS_OK && dir) return RP_FS_OK;
    return rp_fs_fault(f,path,"cannot be made a directory",err);
}

/* A file or an empty directory removed. */
static RpFs rp_fs_remove(const char *path, RpFault *f) {
    if(remove(path)==0) return RP_FS_OK;
    return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,path,"cannot be removed",errno);
}

/* A path's permission bits set: the self-test makes a directory that can
 * be searched and not listed. */
static RpFs rp_fs_mode(const char *path, unsigned mode, RpFault *f) {
    return chmod(path,(mode_t)mode)==0 ? RP_FS_OK : rp_fs_fault(f,path,"cannot have its mode set",errno);
}

static int rp_fs_strcmp(const void *a, const void *b) { return strcmp(*(char *const *)a,*(char *const *)b); }

/* The names in `dir` that end in `suffix`, sorted, into `*names` (each and
 * the array malloc'd; rp_fs_names_free lets them go). A directory that is
 * there and cannot be listed whole is a fault. */
static RpFs rp_fs_list(const char *dir, const char *suffix, char ***names, size_t *n, RpFault *f) {
    *names=NULL; *n=0;
    DIR *d=opendir(dir);
    if(d==NULL) return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,dir,"cannot be listed",errno);
    size_t want=strlen(suffix);
    for(;;) {
        errno=0;
        struct dirent *e=readdir(d);
        if(e==NULL) {
            int err=errno;
            closedir(d);
            if(err) {
                for(size_t i=0;i<*n;i++) free((*names)[i]);
                free(*names); *names=NULL; *n=0;
                return rp_fs_fault(f,dir,"cannot be listed whole",err);
            }
            break;
        }
        size_t len=strlen(e->d_name);
        if(len>want && !strcmp(e->d_name+len-want,suffix)) {
            *names=realloc(*names,(*n+1)*sizeof(**names));
            (*names)[(*n)++]=strdup(e->d_name);
        }
    }
    if(*n>0) qsort(*names,*n,sizeof(**names),rp_fs_strcmp);
    return RP_FS_OK;
}
static void rp_fs_names_free(char **names, size_t n) {
    for(size_t i=0;i<n;i++) free(names[i]);
    free(names);
}
