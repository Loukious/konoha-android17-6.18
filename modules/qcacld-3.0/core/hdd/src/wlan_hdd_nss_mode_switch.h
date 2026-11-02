#ifndef _WLAN_HDD_NSS_MODE_SWITCH_H
#define _WLAN_HDD_NSS_MODE_SWITCH_H

#include "wlan_hdd_main.h"

struct nss_mode_context {
	char intf_name[IFNAMSIZ];
	struct nss_mode_context *next;
	bool is_nss_2x2;
};

int wlan_hdd_set_nss_and_antenna_mode(struct wlan_hdd_link_info *link_info,
				      int nss, int mode);
void wlan_hdd_stop_nss_mode_switch(struct hdd_adapter *adapter);

#endif /* _WLAN_HDD_NSS_MODE_SWITCH_H */
