#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-pointer-to-int-cast -ffunction-sections -fdata-sections -Ikernel lib/datetime.c drivers/rtc.c net/sntp.c net/time_dns.c tests/calendar.c -Wl,--gc-sections -o build/calendar-test
./build/calendar-test
cc -std=c11 -O2 -Wall -Wextra -Werror -Ikernel kernel/clock.c lib/datetime.c net/sntp.c net/time_dns.c tests/clock_service.c -o build/clock-service-test
./build/clock-service-test
