.section .text.entry
.code32
.global _entry
.extern kmain, __bss_start, __bss_end, __bss_low_end, __bss_high_start
_entry:
 cld
 movl $__bss_start,%edi
 movl $__bss_low_end,%ecx
 subl %edi,%ecx
 xorl %eax,%eax
 rep stosb
 movl $__bss_high_start,%edi
 movl $__bss_end,%ecx
 subl %edi,%ecx
 rep stosb
 call kmain
 cli
1: hlt
 jmp 1b
.section .text
.global trap_common
trap_common:
 pushal
 pushl %ds
 pushl %es
 pushl %fs
 pushl %gs
 movw $16,%ax
 movw %ax,%ds
 movw %ax,%es
 movw %ax,%fs
 movw %ax,%gs
 cld
 pushl %esp
 call trap_dispatch
 addl $4,%esp
 cli
 movl %eax,%esp
 popl %gs
 popl %fs
 popl %es
 popl %ds
 popal
 addl $8,%esp
 iret
.global isr_0
isr_0:
 pushl $0
 pushl $0
 jmp trap_common
.global isr_1
isr_1:
 pushl $0
 pushl $1
 jmp trap_common
.global isr_2
isr_2:
 pushl $0
 pushl $2
 jmp trap_common
.global isr_3
isr_3:
 pushl $0
 pushl $3
 jmp trap_common
.global isr_4
isr_4:
 pushl $0
 pushl $4
 jmp trap_common
.global isr_5
isr_5:
 pushl $0
 pushl $5
 jmp trap_common
.global isr_6
isr_6:
 pushl $0
 pushl $6
 jmp trap_common
.global isr_7
isr_7:
 pushl $0
 pushl $7
 jmp trap_common
.global isr_8
isr_8:
 pushl $8
 jmp trap_common
.global isr_9
isr_9:
 pushl $0
 pushl $9
 jmp trap_common
.global isr_10
isr_10:
 pushl $10
 jmp trap_common
.global isr_11
isr_11:
 pushl $11
 jmp trap_common
.global isr_12
isr_12:
 pushl $12
 jmp trap_common
.global isr_13
isr_13:
 pushl $13
 jmp trap_common
.global isr_14
isr_14:
 pushl $14
 jmp trap_common
.global isr_15
isr_15:
 pushl $0
 pushl $15
 jmp trap_common
.global isr_16
isr_16:
 pushl $0
 pushl $16
 jmp trap_common
.global isr_17
isr_17:
 pushl $17
 jmp trap_common
.global isr_18
isr_18:
 pushl $0
 pushl $18
 jmp trap_common
.global isr_19
isr_19:
 pushl $0
 pushl $19
 jmp trap_common
.global isr_20
isr_20:
 pushl $0
 pushl $20
 jmp trap_common
.global isr_21
isr_21:
 pushl $21
 jmp trap_common
.global isr_22
isr_22:
 pushl $0
 pushl $22
 jmp trap_common
.global isr_23
isr_23:
 pushl $0
 pushl $23
 jmp trap_common
.global isr_24
isr_24:
 pushl $0
 pushl $24
 jmp trap_common
.global isr_25
isr_25:
 pushl $0
 pushl $25
 jmp trap_common
.global isr_26
isr_26:
 pushl $0
 pushl $26
 jmp trap_common
.global isr_27
isr_27:
 pushl $0
 pushl $27
 jmp trap_common
.global isr_28
isr_28:
 pushl $0
 pushl $28
 jmp trap_common
.global isr_29
isr_29:
 pushl $29
 jmp trap_common
.global isr_30
isr_30:
 pushl $30
 jmp trap_common
.global isr_31
isr_31:
 pushl $0
 pushl $31
 jmp trap_common
.global isr_32
isr_32:
 pushl $0
 pushl $32
 jmp trap_common
.global isr_33
isr_33:
 pushl $0
 pushl $33
 jmp trap_common
.global isr_34
isr_34:
 pushl $0
 pushl $34
 jmp trap_common
.global isr_35
isr_35:
 pushl $0
 pushl $35
 jmp trap_common
.global isr_36
isr_36:
 pushl $0
 pushl $36
 jmp trap_common
.global isr_37
isr_37:
 pushl $0
 pushl $37
 jmp trap_common
.global isr_38
isr_38:
 pushl $0
 pushl $38
 jmp trap_common
.global isr_39
isr_39:
 pushl $0
 pushl $39
 jmp trap_common
.global isr_40
isr_40:
 pushl $0
 pushl $40
 jmp trap_common
.global isr_41
isr_41:
 pushl $0
 pushl $41
 jmp trap_common
.global isr_42
isr_42:
 pushl $0
 pushl $42
 jmp trap_common
.global isr_43
isr_43:
 pushl $0
 pushl $43
 jmp trap_common
.global isr_44
isr_44:
 pushl $0
 pushl $44
 jmp trap_common
.global isr_45
isr_45:
 pushl $0
 pushl $45
 jmp trap_common
.global isr_46
isr_46:
 pushl $0
 pushl $46
 jmp trap_common
.global isr_47
isr_47:
 pushl $0
 pushl $47
 jmp trap_common
.global isr_128
isr_128:
 pushl $0
 pushl $128
 jmp trap_common
.global isr_129
isr_129:
 pushl $0
 pushl $129
 jmp trap_common
.section .rodata
.global isr_table
isr_table:
 .long isr_0
 .long isr_1
 .long isr_2
 .long isr_3
 .long isr_4
 .long isr_5
 .long isr_6
 .long isr_7
 .long isr_8
 .long isr_9
 .long isr_10
 .long isr_11
 .long isr_12
 .long isr_13
 .long isr_14
 .long isr_15
 .long isr_16
 .long isr_17
 .long isr_18
 .long isr_19
 .long isr_20
 .long isr_21
 .long isr_22
 .long isr_23
 .long isr_24
 .long isr_25
 .long isr_26
 .long isr_27
 .long isr_28
 .long isr_29
 .long isr_30
 .long isr_31
 .long isr_32
 .long isr_33
 .long isr_34
 .long isr_35
 .long isr_36
 .long isr_37
 .long isr_38
 .long isr_39
 .long isr_40
 .long isr_41
 .long isr_42
 .long isr_43
 .long isr_44
 .long isr_45
 .long isr_46
 .long isr_47
.section .note.GNU-stack,"",@progbits
