#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Shared HTTP redirect / browser resource reference joining. Preserve the
 * complete authority (including port) and never silently truncate a URL. */
bool net_url_resolve(const char *base,const char *ref,char *out,size_t cap) {
    if(!base || !ref || !out || !cap) return false;
    out[0]=0;
    const char *colon=strchr(ref,':');
    const char *delim=strpbrk(ref,"/?#");
    if(colon && (!delim || colon<delim)) {
        if(strncmp(ref,"http://",7) && strncmp(ref,"https://",8) &&
           strncmp(ref,"file://",7) && strncmp(ref,"luna://",7) && strncmp(ref,"about:",6))
            return false;
        size_t n=strlen(ref);if(n>=cap) return false;
        memcpy(out,ref,n+1);return true;
    }
    colon=strchr(base,':');
    if(!colon || colon[1]!='/' || colon[2]!='/') return false;
    const char *authority=colon+3,*end=authority;
    while(*end && *end!='/' && *end!='?' && *end!='#') end++;
    size_t prefix=end-base;
    bool slash=false;
    if(!*ref || *ref=='#') {
        const char *fragment=strchr(base,'#');
        prefix=fragment ? (size_t)(fragment-base) : strlen(base);
    } else if(ref[0]=='/' && ref[1]=='/') {
        prefix=colon-base+1; /* scheme + ':', not scheme + '://' */
    } else if(*ref=='?') {
        const char *query=strpbrk(end,"?#");
        prefix=query ? (size_t)(query-base) : strlen(base);
    } else if(*ref!='/') {
        const char *last=NULL;
        for(const char *p=end;*p && *p!='?' && *p!='#';p++) if(*p=='/') last=p;
        if(last) prefix=last-base+1;
        else slash=true;
    }
    size_t n=strlen(ref);
    if(prefix>=cap || n>=cap-prefix || (slash && n+1>=cap-prefix)) return false;
    memcpy(out,base,prefix);
    if(slash) out[prefix++]='/';
    memcpy(out+prefix,ref,n+1);return true;
}
