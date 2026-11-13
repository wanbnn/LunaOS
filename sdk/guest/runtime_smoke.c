#include "posix.h"
#include <stddef.h>
#include <stdint.h>
void *malloc(size_t n);
void free(void *);
void *realloc(void *, size_t);
void *calloc(size_t n,size_t bytes);

static int atoi_small(const char *s) {
    int n=0;
    for(;*s;s++) {
        if(*s<'0'||*s>'9')return -1;
        n=n*10+*s-'0';
    }
    return n;
}
int main(int argc, char **argv, char **envp) {
    if (argc==3 && argv[1][0]=='C') {
        int fd=atoi_small(argv[2]);
        if(fd<3)return 131;
        static const char marker[]="CHILD PIPE WORKS";
        return write(fd,marker,sizeof(marker)-1u)==(long)(sizeof(marker)-1u) ?
            0 : 132;
    }
    /* A proper syscall-backed heap rather than the earlier static 512 KiB
     * bump buffer. Allocate, resize, free, and reuse real mapped pages. */
    unsigned char *buf=malloc(64u*1024u);
    if(!buf)return 133;
    for(unsigned i=0;i<64u*1024u;i++)buf[i]=(unsigned char)(i&255u);
    unsigned char *grown=realloc(buf,256u*1024u);
    if(!grown)return 134;
    for(unsigned i=0;i<64u*1024u;i++)
        if(grown[i]!=(unsigned char)(i&255u))return 135;
    free(grown);
    void *reused=malloc(64u*1024u);
    if(!reused || reused!=(void *)grown)return 136;
    free(reused);
    unsigned char *zero=calloc(8192u,8u);
    if(!zero)return 137;
    for(unsigned i=0;i<8192u*8u;i++)
        if(zero[i])return 138;
    free(zero);
    int pair[2];
    if(pipe(pair))return 139;
    char writefd[8]={0};
    writefd[0]=(char)('0'+pair[1]/10);
    writefd[1]=(char)('0'+pair[1]%10);
    const char *args[]={"/tmp/runtime.elf","Child",writefd,0};
    int pid=luna_spawn("/tmp/runtime.elf",args,(const char *const *)envp);
    if(pid<1)return 140;
    if(close(pair[1]))return 141;
    char received[sizeof("CHILD PIPE WORKS")]={0};
    long bytes=read(pair[0],received,sizeof("CHILD PIPE WORKS")-1u);
    if(bytes!=(long)(sizeof("CHILD PIPE WORKS")-1u))return 142;
    static const char expected[]="CHILD PIPE WORKS";
    for(unsigned i=0;i<sizeof(expected)-1u;i++)
        if(received[i]!=expected[i])return 143;
    if(read(pair[0],received,1)!=0 || close(pair[0]))return 144;
    int code=-1;
    if(luna_waitpid(pid,&code,0)!=pid || code)return 145;
    static const char ok[]="LUNA HEAP AND IPC GUEST OK\n";
    return write(1,ok,sizeof(ok)-1u)==(long)(sizeof(ok)-1u) ? 0 : 146;
}
