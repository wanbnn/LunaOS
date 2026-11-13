.section .text.start,"ax",@progbits
.code32
.global _start
.extern main
_start:
    /* Segment-relative ESP from LunaOS: argc/argv/envp SysV-like stack. */
    movl (%esp), %eax
    leal 4(%esp), %ebx
    leal 8(%esp,%eax,4), %ecx
    pushl %ecx
    pushl %ebx
    pushl %eax
    call main
    movl %eax, %ebx
    movl $3, %eax
    int $0x80
1:  jmp 1b
.section .note.GNU-stack,"",@progbits
