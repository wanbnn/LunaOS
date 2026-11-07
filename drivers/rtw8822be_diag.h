#ifndef LUNA_RTW8822BE_DIAG_H
#define LUNA_RTW8822BE_DIAG_H
#include <stddef.h>
/* Read-only hardware smoke diagnostic; never enables radio/DMA. */
int rtw8822be_diag(char *out, size_t cap);
#endif
