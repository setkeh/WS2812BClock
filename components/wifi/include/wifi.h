#pragma once
#include <stdbool.h>


bool wifi_init_sta(void);

// The device's name on the network, and the single source of identity for
// this firmware: the DHCP lease, the syslog HOSTNAME field and the filename a
// crash dump is stored under all come from here.
//
// Set with CONFIG_LWIP_LOCAL_HOSTNAME, in menuconfig under
// Component config -> LWIP -> Local netif hostname. Returns "unknown" before
// the station netif exists.
const char *wifi_hostname(void);
