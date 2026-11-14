.text
.global __unmapself
.hidden __unmapself
.type __unmapself,@function
__unmapself:
    /* Single-thread process exit; kernel releases its entire address space. */
    movl $3,%eax
    xorl %ebx,%ebx
    int $0x80
    ud2
.section .note.GNU-stack,"",@progbits
