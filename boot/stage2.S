/* BIOS bounce-buffer loader: copy chunks above 1 MiB, avoiding the conventional RAM limit. */
.code16
.section .text
.global _start
_start:
 xorw %ax,%ax
 movw %ax,%ds
 movw %ax,%es
 movb %dl,drive
 movb %dl,0x6f00
 movl 0x7db8,%eax
 movl %eax,0x6f04
 movw 0x7dbc,%ax
 movw %ax,0x6f08
 movl 0x7db0,%eax
 movl %eax,0x6f0c
 movw $0x6000,%di
 movw $0x118,%cx
 movw $0x4f01,%ax
 int $0x10
 cmpw $0x004f,%ax
 jne fail
 movw $0x4118,%bx
 movw $0x4f02,%ax
 int $0x10
 cmpw $0x004f,%ax
 jne fail
 /* EDID is advisory; unverified refresh rates will not be exposed as hardware modes. */
 xorw %ax,%ax
 movw %ax,%es
 movw $0x6800,%di
 movw $0x4f15,%ax
 movw $1,%bx
 xorw %cx,%cx
 xorw %dx,%dx
 int $0x10
 xorw %ax,%ax
 movw %ax,%es
 movw $0x5004,%di
 xorl %ebx,%ebx
 movl $0,0x5000
memory_map:
 movl $0xe820,%eax
 movl $0x534d4150,%edx
 movl $24,%ecx
 movl $1,%es:20(%di)
 int $0x15
 jc map_done
 cmpl $0x534d4150,%eax
 jne map_done
 incl 0x5000
 addw $24,%di
 cmpl $64,0x5000
 jae map_done
 testl %ebx,%ebx
 jnz memory_map
map_done:
 cli
 inb $0x92,%al
 orb $2,%al
 outb %al,$0x92
read_chunk:
 xorw %ax,%ax
 movw %ax,%ds
 movw %ax,%es
 movl remaining,%eax
 cmpl $64,%eax
 jbe small_chunk
 movl $64,%eax
small_chunk:
 cmpw $0xcd01,0x6f08
 jne chunk_ready
 movl packet+8,%ebx
 andl $31,%ebx
 movl $32,%ecx
 subl %ebx,%ecx
 cmpl %ecx,%eax
 jbe chunk_ready
 movl %ecx,%eax
chunk_ready:
 movw %ax,packet+2
 movw $packet,%si
 movb drive,%dl
 sti
 cmpw $0xcd01,0x6f08
 je read_cd_chs
 movb $0x42,%ah
 int $0x13
 jc fail
 jmp read_complete
read_cd_chs:
 movl packet+8,%eax
 xorl %edx,%edx
 movl $32,%ebx
 divl %ebx
 movb %dl,%cl
 incb %cl
 xorl %edx,%edx
 movl $64,%ebx
 divl %ebx
 movb %al,%ch
 movb %dl,%dh
 movb drive,%dl
 movw $0x1000,%ax
 movw %ax,%es
 xorw %bx,%bx
 movb packet+2,%al
 movb $2,%ah
 int $0x13
 jc fail
read_complete:
 cli
 lgdt gdt_descriptor
 movl %cr0,%eax
 orl $1,%eax
 movl %eax,%cr0
 ljmpl $8,$copy_chunk
fail:
 movw $0x0e45,%ax
 int $0x10
 cli
1: hlt
 jmp 1b
.align 8
gdt:
 .quad 0
 .quad 0x00cf9a000000ffff
 .quad 0x00cf92000000ffff
 .quad 0x00009a000000ffff
 gdt_end:
gdt_descriptor:
 .word gdt_end-gdt-1
 .long gdt
.align 4
packet: .byte 16,0
 .word 64,0,0x1000
 .long 65,0
remaining: .long KERNEL_SECTORS
destination: .long 0x100000
drive: .byte 0
.code32
copy_chunk:
 movw $16,%ax
 movw %ax,%ds
 movw %ax,%es
 movw %ax,%ss
 movl $0x10000,%esi
 movl destination,%edi
 movzwl packet+2,%ecx
 movl %ecx,%ebx
 shll $7,%ecx
 cld
 rep movsl
 movl %edi,destination
 addl %ebx,packet+8
 subl %ebx,remaining
 jz boot_kernel
 ljmp $24,$return16
boot_kernel:
 movw $16,%ax
 movw %ax,%fs
 movw %ax,%gs
 movl $0x3f00000,%esp
 movl $0x100000,%eax
 jmp *%eax
.code16
return16:
 movl %cr0,%eax
 andl $0xfffffffe,%eax
 movl %eax,%cr0
 ljmp $0,$real_resume
real_resume:
 xorw %ax,%ax
 movw %ax,%ds
 movw %ax,%es
 movw %ax,%ss
 movw $0x7c00,%sp
 jmp read_chunk
