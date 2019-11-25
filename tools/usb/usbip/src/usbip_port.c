// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 */

#include <getopt.h>

#include "vhci_driver.h"
#include "usbip_common.h"
#include "usbip_enumerate.h"
#include "utils.h"

static const char usbip_port_usage_string[] =
	"usbip port <args>\n"
	"    -i, --vhci-ix=<ix>   index of the "
	USBIP_VHCI_DRV_NAME
	" the device is on (defaults to 0)\n"
	"    -a, --all            list the ports from all the available "
	USBIP_VHCI_DRV_NAME "'s\n";

void usbip_port_usage(void)
{
	printf("usage: %s", usbip_port_usage_string);
}

static int list_imported_devices(void)
{
	int i;
	struct usbip_imported_device *idev;
	int ret = 0;

	for (i = 0; i < vhci_driver->nports; i++) {
		idev = &vhci_driver->idev[i];

		if (usbip_vhci_imported_device_dump(idev) < 0) {
			err("unable to list device %d", i);
			ret = -1;
		}
	}
	return ret;
}

static void list_imported_devices_header(void)
{
	printf("Imported USB devices\n");
	printf("====================\n");
}

static int list_imported_devices_ix(int vhci_ix)
{
	int ret;
	if (usbip_names_init(USBIDS_FILE))
		err("failed to open %s", USBIDS_FILE);

	ret = usbip_vhci_driver_open_ix(vhci_ix);
	if (ret < 0) {
		err("open vhci_driver");
		goto err_names_free;
	}

	list_imported_devices_header();
	ret = list_imported_devices();
	usbip_vhci_driver_close();
err_names_free:
	usbip_names_free();
	return ret;
}

static int list_imported_devices_all(void)
{
	struct udev_enumerate *enumerate = NULL;
	struct udev_list_entry *list, *entry;
	int rc = 0;

	if (usbip_names_init(USBIDS_FILE))
		err("failed to open %s", USBIDS_FILE);

	enumerate = vhci_enumerate();
	if (!enumerate) {
		err("Unable to list vhci_hcd drivers");
		return -1;
	}

	list = udev_enumerate_get_list_entry(enumerate);
	if (!list) {
		err("Unable to list vhci_hcd drivers");
		return -1;
	}

	list_imported_devices_header();

	udev_list_entry_foreach(entry, list) {
		const char *path = udev_list_entry_get_name(entry);
		int i;
		int len = printf("VHCI: %s\n", path);

		/* write a line of dashes */
		for (i = 1; i < len; i++)
			putchar('-');
		putchar('\n');

		if (usbip_vhci_driver_open_path(path) < 0) {
			err("usbip_vhci_driver_open_path");
			rc = -1;
			continue;
		}
		if (list_imported_devices() < 0)
			rc = -1;
		usbip_vhci_driver_close();
	}
	usbip_names_free();
	udev_enumerate_unref(enumerate);

	return rc;
}

int usbip_port_show(__attribute__((unused)) int argc,
		    __attribute__((unused)) char *argv[])
{
	int vhci_ix = 0;
	int all = 0;
	static const struct option opts[] = {
		{ "vhci-ix", 0, NULL, 'i' },
		{ "all", 0, NULL, 'a' },
		{ NULL, 0, NULL, 0 }
	};

	for (;;) {
		int opt = getopt_long(argc, argv, "i:a", opts, NULL);

		if (opt == -1)
			break;
		switch (opt) {
		case 'i':
			if (atoi_with_check(optarg, &vhci_ix) < 0) {
				err("Bad vhci index");
				return -1;
			}
			break;
		case 'a':
			all = 1;
			break;
		default:
			goto err_out;
		}
	}

	if (optind < argc)
		goto err_out;

	if (all)
		return list_imported_devices_all();
	else
		return list_imported_devices_ix(vhci_ix);

err_out:
	usbip_port_usage();
	return -1;
}
