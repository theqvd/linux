/*
 * Copyright (C) 2018 Qindel Formación y Servicios SL
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <libudev.h>

#include "usbip_enumerate.h"
#include "vhci_driver.h"

struct udev_enumerate *vhci_enumerate(void)
{
	struct udev *udev_context = NULL;
	struct udev_enumerate *enumerate = NULL;
	int rc;

	udev_context = udev_new();
	if (!udev_context) {
		err("udev_new failed");
		return NULL;
	}

	enumerate = udev_enumerate_new(udev_context);
	if (!enumerate) {
		err("udev_enumerate_new failed");
		goto err;
	}

	udev_enumerate_add_match_subsystem(enumerate, USBIP_VHCI_BUS_TYPE);
	udev_enumerate_add_match_sysname(enumerate,
					 USBIP_VHCI_DEVICE_NAME_PATTERN);
	rc = udev_enumerate_scan_devices(enumerate);
	if (rc < 0) {
		err("udev_enumerate_scan_devices failed: %d", rc);
		udev_enumerate_unref(enumerate);
		enumerate = NULL;
	}

err:
	udev_unref(udev_context);

	return enumerate;
}
