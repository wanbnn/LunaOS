.text
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.type __syscall_cp_asm,@function
__syscall_cp_asm:
__cp_begin:
    movl 4(%esp),%eax
    cmpl $0,(%eax)
    jne __cp_cancel
    /* Forward nr,a,b,c,d,e,f, omitting the cancellation pointer. */
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    pushl 32(%esp)
    call __luna_syscall
    addl $28,%esp
__cp_end:
    ret
__cp_cancel:
    movl $-4,%eax
    ret
.section .note.GNU-stack,"",@progbits
