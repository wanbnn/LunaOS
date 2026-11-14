.text
.global __clone
.hidden __clone
.type __clone,@function
__clone:
    /* Native pthreads/TLS are not available. Never issue Linux clone(120). */
    movl $-38,%eax
    ret
.section .note.GNU-stack,"",@progbits
