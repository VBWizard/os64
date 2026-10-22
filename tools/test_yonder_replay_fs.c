/* tools/test_yonder_replay_fs.h says what this is: the replay runner's
 * door to the host's files, in a translation unit of its own so the
 * runner's can poison every call it makes. */
#define _GNU_SOURCE
#include "test_yonder_replay_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static RpFs rp_fs_fault(RpFault *f, const char *path, const char *what, int err) {
    snprintf(f->why,sizeof(f->why),"%s %s: %s",path,what,strerror(err));
    return RP_FS_FAULT;
}

RpFs rp_fs_read(const char *path, char **bytes, size_t *len, RpFault *f) {
    *bytes=NULL; if(len!=NULL) *len=0;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0 && errno==ELOOP) return rp_fs_fault(f,path,"is a symbolic link, which nothing kept is",errno);
    if(fd<0) return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,path,"cannot be opened",errno);
    FILE *file=fdopen(fd,"rb");
    if(file==NULL) { int err=errno; close(fd); return rp_fs_fault(f,path,"cannot be opened",err); }
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

RpFs rp_fs_write(const char *path, const void *bytes, size_t len, RpFault *f) {
    char temp[PATH_MAX];
    int t=snprintf(temp,sizeof(temp),"%s.%ld.new",path,(long)getpid());
    if(t<0 || (size_t)t>=sizeof(temp)) return rp_fs_fault(f,path,"has no room for a temporary name",ENAMETOOLONG);
    /* Made new or not at all: a name already there, a link planted at it
     * included, is a fault, never a file to write through. */
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0666);
    if(fd<0) return rp_fs_fault(f,temp,errno==EEXIST ? "is there already" : "cannot be made",errno);
    FILE *file=fdopen(fd,"wb");
    if(file==NULL) { int err=errno; close(fd); remove(temp); return rp_fs_fault(f,temp,"cannot be made",err); }
    errno=0;
    bool ok=fwrite(bytes,1,len,file)==len && fflush(file)==0 && fsync(fileno(file))==0;
    int err=errno;
    if(fclose(file)!=0 && ok) { ok=false; err=errno; }
    if(ok && rename(temp,path)!=0) { ok=false; err=errno; }
    if(!ok) {
        remove(temp);
        return rp_fs_fault(f,path,"cannot be written",err ? err : EIO);
    }
    return RP_FS_OK;
}

/* What is at `path`: a directory (`*dir` true) or anything else. */
RpFs rp_fs_kind(const char *path, bool *dir, RpFault *f) {
    struct stat st;
    if(lstat(path,&st)!=0) return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,path,"cannot be looked at",errno);
    if(S_ISLNK(st.st_mode)) return rp_fs_fault(f,path,"is a symbolic link, which nothing kept is",ELOOP);
    *dir=S_ISDIR(st.st_mode);
    return RP_FS_OK;
}

/* A directory made, or one already there. */
RpFs rp_fs_mkdir(const char *path, RpFault *f) {
    if(mkdir(path,0777)==0) return RP_FS_OK;
    int err=errno;
    struct stat st;
    if(err==EEXIST && stat(path,&st)==0 && S_ISDIR(st.st_mode)) return RP_FS_OK;
    return rp_fs_fault(f,path,"cannot be made a directory",err);
}

/* A file or an empty directory removed. */
RpFs rp_fs_remove(const char *path, RpFault *f) {
    if(remove(path)==0) return RP_FS_OK;
    return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,path,"cannot be removed",errno);
}

RpFs rp_fs_symlink(const char *target, const char *path, RpFault *f) {
    return symlink(target,path)==0 ? RP_FS_OK : rp_fs_fault(f,path,"cannot be made a link",errno);
}

/* A path's permission bits set: the self-test makes a directory that can
 * be searched and not listed. */
RpFs rp_fs_mode(const char *path, unsigned mode, RpFault *f) {
    return chmod(path,(mode_t)mode)==0 ? RP_FS_OK : rp_fs_fault(f,path,"cannot have its mode set",errno);
}

RpFs rp_fs_same(const char *a, const char *b, bool *same, RpFault *f) {
    struct stat x, y;
    if(stat(a,&x)!=0) return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,a,"cannot be looked at",errno);
    if(stat(b,&y)!=0) return errno==ENOENT ? RP_FS_ABSENT : rp_fs_fault(f,b,"cannot be looked at",errno);
    *same=x.st_dev==y.st_dev && x.st_ino==y.st_ino;
    return RP_FS_OK;
}

static int rp_fs_strcmp(const void *a, const void *b) { return strcmp(*(char *const *)a,*(char *const *)b); }

/* The names in `dir` that end in `suffix`, sorted, into `*names` (each and
 * the array malloc'd; rp_fs_names_free lets them go). A directory that is
 * there and cannot be listed whole is a fault. */
RpFs rp_fs_list(const char *dir, const char *suffix, char ***names, size_t *n, RpFault *f) {
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
        if(len>=want && !strcmp(e->d_name+len-want,suffix)) {
            *names=realloc(*names,(*n+1)*sizeof(**names));
            (*names)[(*n)++]=strdup(e->d_name);
        }
    }
    if(*n>0) qsort(*names,*n,sizeof(**names),rp_fs_strcmp);
    return RP_FS_OK;
}
void rp_fs_names_free(char **names, size_t n) {
    for(size_t i=0;i<n;i++) free(names[i]);
    free(names);
}
