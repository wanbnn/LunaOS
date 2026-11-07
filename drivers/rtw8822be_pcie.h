#ifndef LUNA_RTW8822BE_PCIE_H
#define LUNA_RTW8822BE_PCIE_H
#include <stdbool.h>
#include <stddef.h>
void rtw8822be_init(void);
bool rtw8822be_present(void);
void rtw8822be_status(char *,size_t);
#endif
