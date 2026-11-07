#ifndef LUNA_RTW8822BE_DIAG_H
#define LUNA_RTW8822BE_DIAG_H
#include <stddef.h>
/* Read-only hardware smoke diagnostic; never enables radio/DMA. */
int rtw8822be_diag(char *out, size_t cap);
/* Explicit MAC power cycle; not a radio start or Wi-Fi connection. */
int rtw8822be_power_test(char *out, size_t cap);
#endif
