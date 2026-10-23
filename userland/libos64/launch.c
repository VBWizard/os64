#include <stdbool.h>
#include <stddef.h>
#include "os64/launch.h"
#include "os64/conf.h"
#include "os64/slurp.h"
#include "os64/mem.h"
#include "os64/proc.h"
#include "os64/str.h"

static bool absolute(const char *path)
{ return path && path[0]=='/' && path[1] && os64_strlen(path)<OS64_LAUNCH_PATH; }

typedef struct {
    const char *extension;
    char application[OS64_LAUNCH_PATH];
    bool invalid;
} association_t;

static bool association(const char *key,const char *value,void *user)
{
    association_t *a=user;
    bool valid=key && key[0]=='.' && key[1];
    if(valid) for(const char *p=key+1;*p;++p)
        if(*p=='.' || *p=='/' || *p=='\\' || (unsigned char)*p<=32)
            valid=false;
    if(!valid || (*value && !absolute(value))) a->invalid=true;
    else if(a->extension && os64_streq_nocase(key,a->extension))
        os64_strcopy(a->application,sizeof(a->application),value);
    return true; /* Validate the tail even after a matching rule. */
}

const char *os64_launch_extension(const char *file)
{
    if (!absolute(file)) return NULL;
    const char *leaf=file,*extension=NULL;
    for (const char *p=file;*p;++p) {
        if (*p=='/') { leaf=p+1; extension=NULL; }
        else if (*p=='.') extension=p;
    }
    if (!extension || extension==leaf || !extension[1]) return NULL;
    for (const char *p=extension+1;*p;++p)
        if (*p=='\\' || *p=='#' || *p=='=' || (unsigned char)*p<=32) return NULL;
    return extension;
}

static bool associations_valid(const char *text,size_t length,void *user)
{
    (void)user;
    association_t a={0};
    return os64_conf_parse(text,length,association,&a)>=0 && !a.invalid;
}

os64_launch_status_t os64_launch_remember(const char *config,const char *file,
    const char *application)
{
    const char *extension=os64_launch_extension(file);
    if (!config || !*config || os64_strlen(config)>=OS64_LAUNCH_PATH ||
        !extension || !absolute(application)) return OS64_LAUNCH_INVALID;
    size_t n=os64_strlen(application);
    if (application[n-1]==' ' || application[n-1]=='\t') return OS64_LAUNCH_INVALID;
    char source[OS64_CONF_PATH_MAX],target[OS64_CONF_PATH_MAX];
    if (config[0]=='/') {
        os64_strcopy(source,sizeof(source),config);
        os64_strcopy(target,sizeof(target),config);
    } else if (os64_conf_find(config,source,sizeof(source))<0 ||
               os64_conf_target(config,target,sizeof(target))<0)
        return OS64_LAUNCH_CONFIG_ERROR;
    os64_conf_pair_t pair={extension,application};
    return os64_conf_merge_checked(source,target,&pair,1,associations_valid,NULL)==0 ?
        OS64_LAUNCH_OK : OS64_LAUNCH_CONFIG_ERROR;
}

os64_launch_status_t os64_launch_association(const char *config,const char *file,
    char out[OS64_LAUNCH_PATH])
{
    if(!out) return OS64_LAUNCH_INVALID;
    *out=0;
    if(!config || !*config || os64_strlen(config)>=OS64_LAUNCH_PATH || !absolute(file))
        return OS64_LAUNCH_INVALID;
    char location[OS64_CONF_PATH_MAX];
    if(config[0]=='/') os64_strcopy(location,sizeof(location),config);
    else if(os64_conf_find(config,location,sizeof(location))<0)
        return OS64_LAUNCH_CONFIG_ERROR;
    association_t a={.extension=os64_launch_extension(file)};
    uint8_t *bytes=NULL; size_t length=0;
    if(os64_slurp(location,OS64_CONF_MAX-1,&bytes,&length)!=OS64_SLURP_OK)
        return OS64_LAUNCH_CONFIG_ERROR;
    int64_t rc=os64_conf_parse((const char *)bytes,length,association,&a);
    os64_free(bytes);
    if(rc<0 || a.invalid) return OS64_LAUNCH_CONFIG_ERROR;
    if(!*a.application) return OS64_LAUNCH_UNASSOCIATED;
    os64_strcopy(out,OS64_LAUNCH_PATH,a.application);
    return OS64_LAUNCH_OK;
}

os64_launch_status_t os64_launch_file(const char *application,const char *file,
    int64_t *pid)
{
    if(pid) *pid=0;
    if(!pid || !absolute(application) || !absolute(file)) return OS64_LAUNCH_INVALID;
    char *argv[]={(char *)application,(char *)file,NULL};
    int64_t child=os64_spawn(application,argv);
    if(child<=0) return OS64_LAUNCH_SPAWN_ERROR;
    *pid=child;
    return OS64_LAUNCH_OK;
}
