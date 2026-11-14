/* Musl-to-LunaOS syscall adapter. Unimplemented operations fail with ENOSYS;
 * they must never accidentally reach another native syscall number. */
#define _GNU_SOURCE 1
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include "kstat.h"
static long native(unsigned n, unsigned a, unsigned b, unsigned c) {
    long r;
    __asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c):"memory","cc");
    return r;
}
void *luna_brk(void *p) { return (void *)(uintptr_t)native(28,(uintptr_t)p,0,0); }
static unsigned process_umask=0022;
static unsigned stdfd_open[3]={1,1,1};
static long io(int fd, void *buf, size_t n, int writing) {
    if (fd>=0 && fd<3) {
        if(!stdfd_open[fd])return -EBADF;
    }
    size_t done=0;
    while (done<n) {
        size_t part=n-done, cap=writing && (fd==1 || fd==2) ? 256 : 4096;
        if (part>cap) part=cap;
        long rc=native(writing?6:8,fd,(uintptr_t)((char *)buf+done),part);
        if (rc<0) return done ? (long)done : rc==-1 ? -EIO : rc;
        done+=(size_t)rc;
        if ((size_t)rc<part || !writing) break;
    }
    return done;
}
typedef struct { uint32_t size,mode,ino,mtime,ctime; } NativeStat;
static int fd_flags[11]={[1]=O_WRONLY,[2]=O_WRONLY};
static long pathstat(const char *path, NativeStat *st) {
    return native(11,(uintptr_t)path,(uintptr_t)st,0) < 0 ? -ENOENT : 0;
}
static long fdstat(int fd, NativeStat *st) {
    if(fd>=0 && fd<3 && !stdfd_open[fd])return -EBADF;
    return native(31,fd,(uintptr_t)st,0)<0 ? -EBADF : 0;
}
static void fill_kstat(struct kstat *out, const NativeStat *st) {
    memset(out,0,sizeof(*out));
    out->st_dev=1; out->st_ino=st->ino; out->__st_ino_truncated=st->ino;
    out->st_mode=st->mode; out->st_nlink=1; out->st_size=st->size;
    out->st_blksize=4096; out->st_blocks=(st->size+511u)/512u;
    out->st_atime_sec=out->st_mtime_sec=st->mtime; out->st_ctime_sec=st->ctime;
}
static long open_native(const char *path, int flags, unsigned mode) {
    NativeStat st;
    long exists=pathstat(path,&st);
    int native_flags=flags & (O_ACCMODE|O_CREAT|O_EXCL|O_TRUNC|O_APPEND);
    if (flags & ~(O_ACCMODE|O_CREAT|O_EXCL|O_TRUNC|O_APPEND|O_CLOEXEC|O_LARGEFILE|O_DIRECTORY|O_NOFOLLOW))
        return -EINVAL;
    if (exists && (flags&O_CREAT)) {
        const char *slash=strrchr(path,'/');
        char parent[128];
        size_t len=slash?(size_t)(slash-path):0;
        if(len>=sizeof(parent))return -ENAMETOOLONG;
        if(slash) { memcpy(parent,path,len);parent[len]=0;if(!len)strcpy(parent,"/"); }
        else strcpy(parent,".");
        NativeStat directory;
        if(pathstat(parent,&directory))return -ENOENT;
        if(!S_ISDIR(directory.mode))return -ENOTDIR;
    }
    if ((flags&O_DIRECTORY) && (exists || !(st.mode&S_IFDIR))) return exists ? exists : -ENOTDIR;
    if (!exists && S_ISDIR(st.mode) && (flags&O_ACCMODE)) return -EISDIR;
    long fd = !exists && S_ISDIR(st.mode) ? native(26,(uintptr_t)path,0,0) :
        native(7,(uintptr_t)path,native_flags,0);
    if (fd<0) return fd==-17 ? -EEXIST : exists && !(flags&O_CREAT) ? -ENOENT : -EIO;
    if(exists && (flags&O_CREAT))native(23,(uintptr_t)path,mode&0777u&~process_umask,0);
    if(fd<3)stdfd_open[fd]=1;
    if (fd<11) fd_flags[fd]=flags;
    if(flags&O_CLOEXEC)native(38,fd,1,0);
    return fd;
}
long __luna_syscall(long n,long a,long b,long c,long d,long e,long f) {
    (void)d;(void)e;(void)f;
    switch(n) {
    case SYS_exit: case SYS_exit_group: native(3,a,0,0); __builtin_unreachable();
    case SYS_write: return io(a,(void *)b,c,1);
    case SYS_read: return io(a,(void *)b,c,0);
    case SYS_writev: case SYS_readv: {
        struct iovec *v=(void *)b;
        long total=0;
        for (long i=0;i<c;i++) {
            long rc=io(a,v[i].iov_base,v[i].iov_len,n==SYS_writev);
            if (rc<0) return total?total:rc;
            total+=rc;
            if ((size_t)rc<v[i].iov_len) break;
        }
        return total;
    }
    case SYS_open: return open_native((void *)a,b,c);
    case SYS_openat:
        if (a!=AT_FDCWD) return -ENOSYS;
        return open_native((void *)b,c,d);
    case SYS_close:
        if(a>=0 && a<3) {
            if(!stdfd_open[a])return -EBADF;
            stdfd_open[a]=0;
        }
        return native(9,a,0,0)<0 ? -EBADF : 0;
    case SYS_getcwd: {
        long rc=native(13,a,b,0);
        return rc<0 ? -ERANGE : (long)strlen((void *)a)+1;
    }
    case SYS_chdir: return native(12,a,0,0)<0 ? -ENOENT : 0;
    case SYS_mkdir: {
        NativeStat st;
        if (!pathstat((void *)a,&st)) return -EEXIST;
        if(native(14,a,b,0)<0)return -ENOENT;
        return native(23,a,b&0777u&~process_umask,0)<0?-EIO:0;
    }
    case SYS_unlink: case SYS_rmdir: return native(15,a,0,0)<0 ? -ENOENT : 0;
    case SYS_rename: return native(16,a,b,0)<0 ? -EIO : 0;
    case SYS_fchmod: return native(41,a,b,0)<0?-EIO:0;
    case SYS_utimensat: case SYS_utimensat_time64: {
        if(d&~AT_SYMLINK_NOFOLLOW)return -ENOSYS;
        if(b && a!=AT_FDCWD && *(char *)b!='/')return -ENOSYS;
        int64_t seconds=0,nsec=UTIME_NOW;
        if(c) {
            if(n==SYS_utimensat_time64) {seconds=((int64_t *)c)[2];nsec=((int64_t *)c)[3];}
            else {seconds=((long *)c)[2];nsec=((long *)c)[3];}
        }
        if(nsec==UTIME_OMIT)return 0;
        if(nsec==UTIME_NOW) {
            uint32_t now[4];if(native(29,0,(uintptr_t)now,0)<0)return -EIO;
            seconds=(uint64_t)now[0]|((uint64_t)now[1]<<32);
        } else if(nsec<0 || nsec>=1000000000)return -EINVAL;
        if(seconds<0 || seconds>UINT32_MAX)return -EOVERFLOW;
        return (b?native(24,b,(uint32_t)seconds,0):native(42,a,(uint32_t)seconds,0))<0?-EIO:0;
    }
    case SYS_chmod: return native(23,a,b&0777,0)<0 ? -ENOENT : 0;
    case SYS_fsync: case SYS_fdatasync: return native(19,a,0,0)<0 ? -EIO : 0;
    case SYS_access: case SYS_faccessat: {
        NativeStat st;
        if (n==SYS_faccessat) { if(a!=AT_FDCWD)return -ENOSYS; a=b; b=c; }
        long rc=pathstat((void *)a,&st);
        if(rc)return rc;
        /* Permission checks for X/R/W use the mode stored by LunaFS. */
        if ((b&X_OK) && !(st.mode&0111))return -EACCES;
        if ((b&R_OK) && !(st.mode&0444))return -EACCES;
        if ((b&W_OK) && !(st.mode&0222))return -EACCES;
        return 0;
    }
    case SYS_stat64: case SYS_lstat64: case SYS_fstat64: case SYS_fstatat64: {
        NativeStat st;
        long rc;
        struct kstat *out=(void *)b;
        if (n==SYS_fstatat64) {
            out=(void *)c;
            if (a==AT_FDCWD) rc=pathstat((void *)b,&st);
            else if ((d&AT_EMPTY_PATH) && !*(char *)b) rc=fdstat(a,&st);
            else return -ENOSYS;
        } else if (n==SYS_fstat64) rc=fdstat(a,&st);
        else rc=pathstat((void *)a,&st);
        if(rc)return rc;
        fill_kstat(out,&st);
        return 0;
    }
    case SYS_pread64: case SYS_pwrite64: {
        if(e || d<0)return -EOVERFLOW;
        long old=native(10,a,0,SEEK_CUR);
        if(old<0)return -EBADF;
        if(native(10,a,d,SEEK_SET)<0)return -EINVAL;
        long rc=io(a,(void *)b,c,n==SYS_pwrite64);
        if(native(10,a,old,SEEK_SET)<0)return -EIO;
        return rc;
    }
    case SYS_lseek: return native(10,a,b,c);
    case SYS__llseek: {
        if (b != 0 && b != -1) return -EOVERFLOW;
        long off=native(10,a,c,e);
        if (off<0)return -EINVAL;
        *(int64_t *)d=off;
        return 0;
    }
    case SYS_fcntl: case SYS_fcntl64:
        if (a<0 || a>=11) return -EBADF;
        NativeStat current;
        if(fdstat(a,&current))return -EBADF;
        switch(b) {
        case F_DUPFD: case F_DUPFD_CLOEXEC: {
            if(c<0 || c>=11)return -EINVAL;
            for(int fd=c;fd<11;fd++) {
                NativeStat st;
                if(!fdstat(fd,&st))continue;
                long rc=native(37,a,fd,0);
                if(rc<0)return rc;
                if(fd<3)stdfd_open[fd]=1;
                fd_flags[fd]=fd_flags[a]&~O_CLOEXEC;
                if(b==F_DUPFD_CLOEXEC){native(38,fd,1,0);fd_flags[fd]|=O_CLOEXEC;}
                return fd;
            }
            return -EMFILE;
        }
        case F_GETFD: {
            long flags=native(38,a,0,2);return flags<0?flags:(flags&O_CLOEXEC)?FD_CLOEXEC:0;
        }
        case F_SETFD:
            if(native(38,a,c&FD_CLOEXEC,0)<0)return -EBADF;
            fd_flags[a]=(fd_flags[a]&~O_CLOEXEC)|((c&FD_CLOEXEC)?O_CLOEXEC:0); return 0;
        case F_GETFL: {
            long flags=native(38,a,0,2);return flags<0?flags:flags&~O_CLOEXEC;
        }
        case F_SETFL:
            if(native(38,a,c&3072u,1)<0)return -EBADF;
            fd_flags[a]=(fd_flags[a]&~3072u)|(c&3072u);return 0;
        default: return -ENOSYS;
        }
    case SYS_getdents64: {
        struct { uint32_t ino,mode,mtime; char name[128]; } entry;
        if(c<(long)sizeof(struct dirent))return -EINVAL;
        long rc=native(27,a,(uintptr_t)&entry,0);
        if(rc<=0)return rc<0?-EBADF:0;
        struct dirent *out=(void *)b;
        size_t bytes=(offsetof(struct dirent,d_name)+strlen(entry.name)+1+7)&~7u;
        memset(out,0,bytes);
        out->d_ino=entry.ino; out->d_off=entry.ino; out->d_reclen=bytes;
        out->d_type=S_ISDIR(entry.mode)?DT_DIR:DT_REG;
        strcpy(out->d_name,entry.name);
        return bytes;
    }
    case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid:
    case SYS_getuid32: case SYS_geteuid32: case SYS_getgid32: case SYS_getegid32:
        return 0;
    case SYS_clock_gettime64: return native(29,a,b,0)<0?-EINVAL:0;
    case SYS_getrandom: {
        size_t amount=b>4096?4096:(size_t)b;
        if(c & ~1u)return -EINVAL;
        return native(30,a,amount,0);
    }
    case SYS_prlimit64:
        if(a && a!=native(17,0,0,0))return -ESRCH;
        if(c)return -ENOSYS;
        if(b!=RLIMIT_NOFILE)return -ENOSYS;
        if(d)((struct rlimit *)d)->rlim_cur=((struct rlimit *)d)->rlim_max=11;
        return 0;
    case SYS_getrlimit: case SYS_ugetrlimit:
        if(a!=RLIMIT_NOFILE)return -ENOSYS;
        ((unsigned long *)b)[0]=((unsigned long *)b)[1]=11;
        return 0;
    case SYS_readlink: case SYS_readlinkat: return -EINVAL;
    case SYS_umask: {
        unsigned old=process_umask;process_umask=a&0777;return old;
    }
    case SYS_execve: {
        NativeStat st;
        if(pathstat((void *)a,&st))return -ENOENT;
        if(!S_ISREG(st.mode))return -EACCES;
        return native(22,a,b,c)<0?-ENOEXEC:0;
    }
    case SYS_wait4:
        if(d)return -ENOSYS;
        return native(21,a,b,c);
    case SYS_poll: return native(43,a,b,c);
    case SYS_pipe: case SYS_pipe2: {
        if(n==SYS_pipe2 && (b&~O_CLOEXEC))return -ENOSYS;
        long rc=native(18,a,0,0);
        if(rc<0)return -EMFILE;
        int *fds=(void *)a;
        if(fds[0]<3)stdfd_open[fds[0]]=1;
        if(fds[1]<3)stdfd_open[fds[1]]=1;
        fd_flags[fds[0]]=O_RDONLY;fd_flags[fds[1]]=O_WRONLY;
        if(n==SYS_pipe2 && (b&O_CLOEXEC)) {
            native(38,fds[0],1,0);native(38,fds[1],1,0);
            fd_flags[fds[0]]|=O_CLOEXEC;fd_flags[fds[1]]|=O_CLOEXEC;
        }
        return 0;
    }
    case SYS_dup: case SYS_dup2: case SYS_dup3: {
        int old=a,next=n==SYS_dup?-1:b;
        if(n==SYS_dup3 && (a==b || (c&~O_CLOEXEC)))return -EINVAL;
        if(a>=0 && a<3 && !stdfd_open[a])return -EBADF;
        long rc=native(37,old,next,0);
        if(rc>=0 && rc<3)stdfd_open[rc]=1;
        if(rc>=0 && rc<11)fd_flags[rc]=old<11?fd_flags[old]&~O_CLOEXEC:0;
        if(rc>=0 && n==SYS_dup3 && (c&O_CLOEXEC))native(38,rc,1,0);
        return rc;
    }
    case SYS_setsid: return native(39,0,0,0);
    case SYS_getppid: return native(39,5,0,0);
    case SYS_getpgrp: return native(39,1,0,0);
    case SYS_getpgid: return native(39,2,a,0);
    case SYS_getsid: return native(39,3,a,0);
    case SYS_setpgid: return native(39,4,a,b);
    case SYS_fork: return native(36,0,0,0);
    case SYS_set_tid_address: return native(17,0,0,0);
    case SYS_rt_sigsuspend:
        if(b!=8)return -EINVAL;
        return native(40,a,0,0);
    case SYS_rt_sigprocmask:
        if(d!=8)return -EINVAL;
        return native(33,a,b,c);
    case SYS_rt_sigaction:
        if(d!=8)return -EINVAL;
        return native(32,a,b,c);
    case SYS_kill: case SYS_tkill: return native(34,a,b,0);
    case SYS_tgkill:
        if(a!=b)return -EINVAL;
        return native(34,b,c,0);
    case SYS_getpid: case SYS_gettid: return native(17,0,0,0);
    case SYS_brk: return native(28,a,0,0);
    case SYS_ioctl: return -ENOTTY;
    default:
        if(getenv("LUNA_TRACE_SYSCALLS")) {
            char msg[64]="LunaOS unsupported syscall ";
            size_t at=strlen(msg);char digits[12];unsigned k=0,v=(unsigned)n;
            do { digits[k++]=(char)('0'+v%10);v/=10; } while(v);
            while(k)msg[at++]=digits[--k];
            msg[at++]='\n';native(6,2,(uintptr_t)msg,at);
        }
        return -ENOSYS;
    }
}
