#ifndef LUNA_ACPI_H
#define LUNA_ACPI_H
#include "luna.h"
void acpi_storage_init(void);
void acpi_storage_status(char *,size_t);
void acpi_battery_poll(void);
void acpi_battery_text(char *, size_t);
#endif
