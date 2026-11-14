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
    if (argc == 2 && argv[1][0] == 'L') {
        int fd = open("/tmp/runtime-test.lock",
                      LUNA_O_WRONLY | LUNA_O_CREAT | LUNA_O_EXCL | LUNA_O_TRUNC);
        return fd == -LUNA_EEXIST ? 0 : 155;
    }
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
    if(!reused || reused!=(void *)buf)return 136;
    free(reused);
    unsigned char *zero=calloc(8192u,8u);
    if(!zero)return 137;
    for(unsigned i=0;i<8192u*8u;i++)
        if(zero[i])return 138;
    free(zero);
    static const char lock_path[] = "/tmp/runtime-test.lock";
    int lock = open(lock_path, LUNA_O_RDWR | LUNA_O_CREAT | LUNA_O_EXCL, 0600);
    if (lock < 0 || write(lock, "lock", 4) != 4) return 147;
    if (open(lock_path, LUNA_O_WRONLY | LUNA_O_CREAT | LUNA_O_EXCL | LUNA_O_TRUNC,
             0600) != -LUNA_EEXIST) return 148;
    const char *lock_args[] = {"/tmp/runtime.elf", "Lock", 0};
    int contender = luna_spawn("/tmp/runtime.elf", lock_args,
                               (const char *const *)envp);
    int lock_status = -1;
    if (contender < 1 || luna_waitpid(contender, &lock_status, 0) != contender ||
        lock_status) return 156;
    /* Index/pack-like header patch, then seek beyond EOF with zero filling. */
    if (lseek(lock, 1, LUNA_SEEK_SET) != 1 || write(lock, "A", 1) != 1 ||
        lseek(lock, 6, LUNA_SEEK_SET) != 6 || write(lock, "Z", 1) != 1 ||
        lseek(lock, 0, LUNA_SEEK_SET) != 0) return 149;
    char content[8] = {0};
    static const char expected_file[] = {'l','A','c','k',0,0,'Z'};
    if (read(lock, content, sizeof(content)) != 7) return 150;
    for (unsigned i = 0; i < sizeof(expected_file); i++)
        if (content[i] != expected_file[i]) return 151;
    if (close(lock)) return 152;
    /* O_APPEND must ignore the seek position for writes. */
    lock = open(lock_path, LUNA_O_WRONLY | LUNA_O_APPEND);
    if (lock < 0 || lseek(lock, 0, LUNA_SEEK_SET) != 0 ||
        write(lock, "!", 1) != 1 || close(lock)) return 153;
    LunaGuestStat info;
    if (luna_stat(lock_path, &info) || info.size != 8 || unlink(lock_path)) return 154;
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
