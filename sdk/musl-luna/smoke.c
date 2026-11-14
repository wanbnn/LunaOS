#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <fcntl.h>
extern long __luna_syscall(long,long,long,long,long,long,long);
static volatile sig_atomic_t delivered;
static void handler(int sig) { if(sig==SIGUSR1)delivered++; }
int main(void) {
    unsigned long limits[4]={0,0,0x12345678u,0xabcdef01u};
    if(__luna_syscall(SYS_ugetrlimit,RLIMIT_NOFILE,(long)limits,0,0,0,0) ||
       limits[0]!=11 || limits[1]!=11 || limits[2]!=0x12345678u || limits[3]!=0xabcdef01u)return 30;
    struct rlimit r;
    if(getrlimit(RLIMIT_NOFILE,&r) || r.rlim_cur!=11 || r.rlim_max!=11)return 31;
    if(signal(SIGUSR1,handler)==SIG_ERR)return 10;
    sigset_t mask;
    sigemptyset(&mask);sigaddset(&mask,SIGUSR1);
    if(sigprocmask(SIG_BLOCK,&mask,NULL) || raise(SIGUSR1) || delivered)return 11;
    if(sigprocmask(SIG_UNBLOCK,&mask,NULL) || delivered!=1)return 12;
    unsigned char *heap=malloc(8192);
    if(!heap)return 13;
    heap[0]=7;
    int pair[2];if(pipe(pair))return 14;
    pid_t pid=fork();if(pid<0)return 15;
    if(!pid) {
        heap[0]=9;
        if(close(STDIN_FILENO) || open("/dev/null",O_RDONLY)!=STDIN_FILENO)_exit(32);
        if(close(pair[0]) || dup2(pair[1],STDOUT_FILENO)!=STDOUT_FILENO || close(pair[1]))_exit(16);
        execl("/bin/sh","sh","-c","git --version; git --version >/dev/null",(char *)NULL);
        _exit(17);
    }
    if(close(pair[1]))return 18;
    char text[256]={0};size_t n=0;
    for(;;) {
        ssize_t rc=read(pair[0],text+n,sizeof(text)-1-n);
        if(rc<0)return 19;
        if(!rc)break;
        n+=(size_t)rc;if(n>=sizeof(text)-1)return 20;
    }
    if(close(pair[0]))return 21;
    int status;
    if(waitpid(pid,&status,0)!=pid || !WIFEXITED(status) || WEXITSTATUS(status))return 22;
    if(heap[0]!=7 || strcmp(text,"git version 2.56.1\n"))return 23;
    free(heap);
    pid=fork();if(pid<0)return 24;
    if(!pid) { raise(SIGTERM);_exit(25); }
    if(waitpid(pid,&status,0)!=pid || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGTERM)return 26;
    puts("LUNA MUSL FORK SIGNAL EXEC OK");
    return 0;
}
