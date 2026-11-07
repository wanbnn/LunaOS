/* Relocatable, standalone x64 -> protected i386 transfer after ExitBootServices. */
.section .text
.code64
.global _start
_start:
 cli
 cld
 movq %rcx,%r12
 movq 16(%r12),%rsp
 movb $0x80,%al
 outb %al,$0x70
 /* Disable BSP local APIC; the current UP kernel owns legacy PIC/PIT. */
 movl $0x1b,%ecx
 rdmsr
 andl $0xfffff3ff,%eax
 wrmsr
 movq %cr4,%rax
 andq $-131201,%rax /* clear PCIDE and PGE, retain PAE while in long mode */
 movq %rax,%cr4
 movq 24(%r12),%rax
 movq %rax,%cr3
 movq 0(%r12),%rsi
 movq 8(%r12),%rcx
 movl $0x100000,%edi
 rep movsb
 movq 32(%r12),%rsi
 movl $0x5000,%edi
 movl $0x2000,%ecx
 rep movsb
 leaq gdt(%rip),%rax
 movq %rax,gdt_ptr+2(%rip)
 lgdt gdt_ptr(%rip)
 pushq $8
 leaq compat32(%rip),%rax
 pushq %rax
 lretq
.code32
compat32:
 movw $16,%ax
 movw %ax,%ds
 movw %ax,%es
 movw %ax,%fs
 movw %ax,%gs
 movw %ax,%ss
 movl %cr0,%eax
 andl $0x7fffffff,%eax
 movl %eax,%cr0
 movl $0xc0000080,%ecx
 rdmsr
 andl $0xfffffeff,%eax
 wrmsr
 movl %cr4,%eax
 andl $0xfffdff5f,%eax
 movl %eax,%cr4
 movl $0x3f00000,%esp
 xorl %ebp,%ebp
 movl $0x100000,%eax
 jmp *%eax
.align 8
gdt:
 .quad 0
 .quad 0x00cf9a000000ffff
 .quad 0x00cf92000000ffff
gdt_end:
gdt_ptr:
 .word gdt_end-gdt-1
 .quad 0
.section .note.GNU-stack,"",@progbits
