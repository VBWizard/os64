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
    const char *leaf=file,*extension=NULL;
    for(const char *p=file;*p;++p) {
        if(*p=='/') { leaf=p+1; extension=NULL; }
        else if(*p=='.') extension=p;
    }
    if(extension==leaf || (extension && !extension[1])) extension=NULL;
    association_t a={.extension=extension};
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
