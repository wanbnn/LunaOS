/* Luna stage 1 reads only its own second stage. The second stage streams the kernel. */
.code16
.section .text
.global _start
_start:
 ljmp $0,$start
start:
 cli
 xorw %ax,%ax
 movw %ax,%ds
 movw %ax,%es
 movw %ax,%ss
 movw $0x7c00,%sp
 sti
 movb %dl,drive
 cmpw $0xcd01,0x7dbc
 je cd_read
 movb $0x41,%ah
 movw $0x55aa,%bx
 int $0x13
 jc error
 cmpw $0xaa55,%bx
 jne error
 testw $1,%cx
 jz error
 movw $packet,%si
 movb drive,%dl
 movb $0x42,%ah
 int $0x13
 jc error
 movb drive,%dl
 ljmp $0,$0x8000
/* El Torito HDD emulation exposes legacy CHS, not EDD, on SeaBIOS. */
cd_read:
 movw $0x800,%ax
 movw %ax,%es
 xorw %bx,%bx
 movw $0x0202,%cx
 xorb %ch,%ch
 xorb %dh,%dh
 movb drive,%dl
 movw $0x021f,%ax
 int $0x13
 jc error
 movw $0x3e00,%bx
 movw $1,%cx
 movb $1,%dh
 movb drive,%dl
 movw $0x0220,%ax
 int $0x13
 jc error
 movw $0x7e00,%bx
 movw $1,%cx
 movb $2,%dh
 movb drive,%dl
 movw $0x0201,%ax
 int $0x13
 jc error
 movb drive,%dl
 ljmp $0,$0x8000
error:
 movw $failure,%si
1: lodsb
 testb %al,%al
 jz halt
 movb $0x0e,%ah
 int $0x10
 jmp 1b
halt: cli
 hlt
 jmp halt
.align 4
packet: .byte 16,0
 .word 64,0,0x800
 .long 1,0
drive: .byte 0
failure: .asciz "Luna boot: EDD read failed."
.org 440
.long 0x4c554e41
.word 0
.org 510
.word 0xaa55
