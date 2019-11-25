// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 */

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getopt.h>
#include <unistd.h>

#include "vhci_driver.h"
#include "usbip_common.h"
#include "usbip_network.h"
#include "usbip.h"
#include "utils.h"

static const char usbip_detach_usage_string[] =
	"usbip detach <args>\n"
	"    -i, --vhci-ix=<ix>   index of the "
	USBIP_VHCI_DRV_NAME
	" the device is on (defaults to 0)\n"
	"    -p, --port=<port>    port the device is on\n";

void usbip_detach_usage(void)
{
	printf("usage: %s", usbip_detach_usage_string);
}

static int detach_port(int vhci_ix, int port)
{
	int ret = 0;
	char path[PATH_MAX+1];
	int i;
	struct usbip_imported_device *idev;
	int found = 0;

	ret = usbip_vhci_driver_open();
	if (ret < 0) {
		err("open vhci_driver");
		return -1;
	}

	/* check for invalid port */
	for (i = 0; i < vhci_driver->nports; i++) {
		idev = &vhci_driver->idev[i];

		if (idev->port == port) {
			found = 1;
			if (idev->status != VDEV_ST_NULL)
				break;
			info("Port %d is already detached!\n", idev->port);
			goto call_driver_close;
		}
	}

	if (!found) {
		err("Invalid port %i > maxports %d",
			port, vhci_driver->nports);
		goto call_driver_close;
	}

	/* remove the port state file */
	snprintf(path, PATH_MAX, VHCI_STATE_PATH"/port%d-%d", vhci_ix, port);

	remove(path);
	rmdir(VHCI_STATE_PATH);

	ret = usbip_vhci_driver_open_ix(vhci_ix);
	ret = usbip_vhci_detach_device(port);
	if (ret < 0) {
		ret = -1;
		err("Port %d detach request failed!\n", port);
		goto call_driver_close;
	}
	info("Port %d is now detached!\n", port);

call_driver_close:
	ret = usbip_vhci_detach_device(port);
	usbip_vhci_driver_close();

	return ret;
}

int usbip_detach(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "port", required_argument, NULL, 'p' },
		{ "vhci-ix", 0, NULL, 'i' },
		{ NULL, 0, NULL, 0 }
	};
	int opt;
	int port = -1;
	int vhci_ix = 0;

	for (;;) {
		opt = getopt_long(argc, argv, "p:i:", opts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'p':
			if (atoi_with_check(optarg, &port) < 0) {
				err("bad port number");
				return -1;
			}
			break;
		case 'i':
			if (atoi_with_check(optarg, &vhci_ix) < 0) {
				err("bad vhci index");
				return -1;
			}
			break;
		default:
			goto err_out;
		}
	}
	if (optind < argc)
		goto err_out;

	if (port < 0)
		goto err_out;

	return detach_port(vhci_ix, port);

err_out:
	usbip_detach_usage();
	return -1;
}
