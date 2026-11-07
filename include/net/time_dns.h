#ifndef LUNA_TIME_DNS_H
#define LUNA_TIME_DNS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool time_hostname_valid(const char *);
int time_dns_question(const char *, uint16_t id, uint8_t *, size_t, unsigned *);
int time_dns_answer(const char *, uint16_t id, const uint8_t *, size_t, uint8_t address[4]);
#endif
