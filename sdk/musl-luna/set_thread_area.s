.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
    movl $-38,%eax
    ret
.section .note.GNU-stack,"",@progbits
