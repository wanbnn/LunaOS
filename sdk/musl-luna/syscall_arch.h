/* Translate musl's internal syscall ABI to LunaOS in userspace. Never issue
 * Linux syscall numbers to LunaOS's unrelated int 0x80 dispatcher. */
#define __SYSCALL_LL_E(x) ((union { long long ll; long l[2]; }){ .ll=(x) }).l[0], ((union { long long ll; long l[2]; }){ .ll=(x) }).l[1]
#define __SYSCALL_LL_O(x) __SYSCALL_LL_E(x)
long __luna_syscall(long, long, long, long, long, long, long);
static inline long __syscall0(long n) { return __luna_syscall(n,0,0,0,0,0,0); }
static inline long __syscall1(long n,long a) { return __luna_syscall(n,a,0,0,0,0,0); }
static inline long __syscall2(long n,long a,long b) { return __luna_syscall(n,a,b,0,0,0,0); }
static inline long __syscall3(long n,long a,long b,long c) { return __luna_syscall(n,a,b,c,0,0,0); }
static inline long __syscall4(long n,long a,long b,long c,long d) { return __luna_syscall(n,a,b,c,d,0,0); }
static inline long __syscall5(long n,long a,long b,long c,long d,long e) { return __luna_syscall(n,a,b,c,d,e,0); }
static inline long __syscall6(long n,long a,long b,long c,long d,long e,long f) { return __luna_syscall(n,a,b,c,d,e,f); }
