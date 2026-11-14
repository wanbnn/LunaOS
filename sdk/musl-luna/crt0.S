.section .text.start,"ax",@progbits
.code32
.global _start
_start:
    xorl %ebp,%ebp
    movl (%esp),%eax
    leal 4(%esp),%ebx
    leal 8(%esp,%eax,4),%ecx
    andl $-16,%esp
    subl $4,%esp
    pushl %ecx
    pushl %ebx
    pushl %eax
    call __luna_start
    ud2
.section .note.GNU-stack,"",@progbits
