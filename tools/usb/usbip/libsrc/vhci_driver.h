/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2005-2007 Takahiro Hirofuchi
 */

#ifndef __VHCI_DRIVER_H
#define __VHCI_DRIVER_H

#include <libudev.h>
#include <stdint.h>

#include "usbip_common.h"

#define USBIP_VHCI_BUS_TYPE "platform"
#define USBIP_VHCI_DEVICE_NAME_PREFIX "vhci_hcd"
#define USBIP_VHCI_DEVICE_NAME_0 (USBIP_VHCI_DEVICE_NAME_PREFIX ".0")
#define USBIP_VHCI_DEVICE_NAME_PATTERN (USBIP_VHCI_DEVICE_NAME_PREFIX ".*")

enum hub_speed {
	HUB_SPEED_HIGH = 0,
	HUB_SPEED_SUPER,
};

struct usbip_imported_device {
	enum hub_speed hub;
	uint8_t port;
	uint32_t status;

	uint32_t devid;

	uint8_t busnum;
	uint8_t devnum;

	/* usbip_class_device list */
	struct usbip_usb_device udev;
};

struct usbip_vhci_driver {

	/* /sys/devices/platform/vhci_hcd */
	struct udev_device *hc_device;

	int ix;
	int nports;
	struct usbip_imported_device idev[];
};


extern struct usbip_vhci_driver *vhci_driver;

/* will be removed */
int usbip_vhci_driver_open(void);

int usbip_vhci_driver_open_path(const char *);
int usbip_vhci_driver_open_ix(int vhci_ix);
void usbip_vhci_driver_close(void);

int  usbip_vhci_refresh_device_list(void);


int usbip_vhci_get_free_port(uint32_t speed);
int usbip_vhci_attach_device2(uint8_t port, int sockfd, uint32_t devid,
		uint32_t speed);

/* will be removed */
int usbip_vhci_attach_device(uint8_t port, int sockfd, uint8_t busnum,
		uint8_t devnum, uint32_t speed);

int usbip_vhci_detach_device(uint8_t port);

int usbip_vhci_imported_device_dump(struct usbip_imported_device *idev);

int usbip_vhci_driver_ix(void);

#endif /* __VHCI_DRIVER_H */
