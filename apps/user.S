/* Position independent native client. Executes on a private user stack at CPL 1/2/3.
 * GUI and OS operations cross int 0x80; no application may issue device I/O directly. */
.section .rodata.user,"a",@progbits
.code32
.global user_program_start,user_program_end
user_program_start:
 movl %eax,%ebp
 xorl %edx,%edx
 movw %cs,%dx
 andl $3,%edx
 xorl %eax,%eax
 int $0x80
 cmpl $1,%ebp
 je probe_io
 cmpl $2,%ebp
 je probe_memory
 cmpl $3,%ebp
 je probe_syscall
 cmpl $4,%ebp
 je probe_segment
client_loop:
 movl $1,%eax
 int $0x80
 /* Kernel only dispatches an event delivered to this task's own mailbox. */
 movl $2,%eax
 int $0x80
 jmp client_loop
probe_io:
 movw $0x80,%dx
 xorb %al,%al
 outb %al,%dx
 jmp probe_failed
probe_memory:
 movl 0x2000,%eax
 jmp probe_failed
probe_segment:
 xorl %edx,%edx
 movw %cs,%dx
 andl $3,%edx
 cmpl $3,%edx
 jne service_address
 /* Segment-base wrap reaches the identity kernel alias. CPL 3 must still
  * fault on its supervisor-only PTE, even on emulators without limit checks. */
 movl 0xc0100000,%eax
 jmp probe_failed
service_address:
 movl 0x100000,%eax
 jmp probe_failed
probe_syscall:
 movl $4,%eax
 int $0x80
 cmpl $-1,%eax
 jne probe_failed
 movl $3,%eax
 xorl %ebx,%ebx
 int $0x80
probe_failed:
 movl $5,%eax
 int $0x80
 jmp probe_failed
user_program_end:
.section .note.GNU-stack,"",@progbits
