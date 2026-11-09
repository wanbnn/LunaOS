#ifndef LUNA_WIFI_PROFILES_H
#define LUNA_WIFI_PROFILES_H
#include "wifi.h"

void wifi_profiles_init(void);
void wifi_profiles_poll(bool save_to_boot_disk);
bool wifi_profiles_write(void);
int wifi_user_scan(void);
int wifi_user_connect(const WifiAp *ap, const char *password);
bool wifi_profile_known(const WifiAp *ap);
#endif
