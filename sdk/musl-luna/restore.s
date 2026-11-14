.global __restore
.hidden __restore
.type __restore,@function
__restore:
.global __restore_rt
.hidden __restore_rt
.type __restore_rt,@function
__restore_rt:
    movl $35,%eax
    int $0x80
    ud2
