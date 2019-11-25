// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 * Copyright (C) 2015-2016 Samsung Electronics
 *               Igor Kotrasinski <i.kotrasinsk@samsung.com>
 *               Krzysztof Opasiak <k.opasiak@samsung.com>
 */

#include <sys/stat.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>

#include "vhci_driver.h"
#include "usbip_common.h"
#include "usbip_network.h"
#include "usbip.h"
#include "usbip_enumerate.h"

static const char usbip_attach_usage_string[] =
	"usbip attach <args>\n"
	"    -r, --remote=<host>      The machine with exported USB devices\n"
	"    -b, --busid=<busid>    Busid of the device on <host>\n"
	"    -d, --device=<devid>    Id of the virtual UDC on <host>\n";

void usbip_attach_usage(void)
{
	printf("usage: %s", usbip_attach_usage_string);
}

#define MAX_BUFF 100
static int record_connection(char *host, char *port,
			     char *busid, int vhci_ix, int rhport)
{
	int fd;
	char path[PATH_MAX+1];
	char buff[MAX_BUFF+1];
	int ret;

	ret = mkdir(VHCI_STATE_PATH, 0700);
	if (ret < 0) {
		/* if VHCI_STATE_PATH exists, then it better be a directory */
		if (errno == EEXIST) {
			struct stat s;

			ret = stat(VHCI_STATE_PATH, &s);
			if (ret < 0)
				return -1;
			if (!(s.st_mode & S_IFDIR))
				return -1;
		} else
			return -1;
	}

	snprintf(path, PATH_MAX, VHCI_STATE_PATH"/port%d-%d", vhci_ix, rhport);

	fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU);
	if (fd < 0)
		return -1;

	snprintf(buff, MAX_BUFF, "%s %s %s\n",
			host, port, busid);

	ret = write(fd, buff, strlen(buff));
	if (ret != (ssize_t) strlen(buff)) {
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static int import_device(int sockfd, struct usbip_usb_device *udev,
			 int *pvhci_ix, int *pport)
{
	uint32_t speed = udev->speed;
	int rc = -1;

	struct udev_enumerate *enumerate;
	struct udev_list_entry *list, *entry;

	enumerate = vhci_enumerate();
	if (!enumerate) {
		err("unable to list vhci_hcd drivers");
		return -1;
	}

	list = udev_enumerate_get_list_entry(enumerate);
	if (!list) {
		err("unable to list vhci_hcd drivers");
		return -1;
	}

	udev_list_entry_foreach(entry, list) {
		const char *path = udev_list_entry_get_name(entry);
		int port, vhci_ix;

		if (usbip_vhci_driver_open_path(path) < 0)
			continue;

		vhci_ix = usbip_vhci_driver_ix();

		/* Between the moment we read and parse the status
		 * files and the one we try to attach a socket to the
		 * port, the later one may become occupied from some
		 * other process. In order to avoid that race
		 * condition, we retry on EBUSY errors. On any other
		 * error we just jump to the next vhci_hcd device
		 */
		while (1) {
			port = usbip_vhci_get_free_port(speed);
			if (port < 0)
				break;

			dbg("got free port %d at %s", port, path);
			rc = usbip_vhci_attach_device(port, sockfd,
						      udev->busnum,
						      udev->devnum,
						      speed);

			if (rc >= 0 || errno != EBUSY)
				break;

			usbip_vhci_refresh_device_list();
		}

		usbip_vhci_driver_close();

		if (rc >= 0) {
			*pport = port;
			*pvhci_ix = vhci_ix;
			goto done;
		}
	}
	err("import device failed");

done:
	udev_enumerate_unref(enumerate);

	return rc;
}

static int query_import_device(int sockfd, char *busid,
			       int *pvhci_ix, int *pport)
{
	int rc;
	struct op_import_request request;
	struct op_import_reply   reply;
	uint16_t code = OP_REP_IMPORT;
	int status;

	memset(&request, 0, sizeof(request));
	memset(&reply, 0, sizeof(reply));

	/* send a request */
	rc = usbip_net_send_op_common(sockfd, OP_REQ_IMPORT, 0);
	if (rc < 0) {
		err("send op_common");
		return -1;
	}

	strncpy(request.busid, busid, SYSFS_BUS_ID_SIZE-1);

	PACK_OP_IMPORT_REQUEST(0, &request);

	rc = usbip_net_send(sockfd, (void *) &request, sizeof(request));
	if (rc < 0) {
		err("send op_import_request");
		return -1;
	}

	/* receive a reply */
	rc = usbip_net_recv_op_common(sockfd, &code, &status);
	if (rc < 0) {
		err("Attach Request for %s failed - %s\n",
		    busid, usbip_op_common_status_string(status));
		return -1;
	}

	rc = usbip_net_recv(sockfd, (void *) &reply, sizeof(reply));
	if (rc < 0) {
		err("recv op_import_reply");
		return -1;
	}

	PACK_OP_IMPORT_REPLY(0, &reply);

	/* check the reply */
	if (strncmp(reply.udev.busid, busid, SYSFS_BUS_ID_SIZE)) {
		err("recv different busid %s", reply.udev.busid);
		return -1;
	}

	/* import a device */
	return import_device(sockfd, &reply.udev, pvhci_ix, pport);
}

static int attach_device(char *host, char *busid)
{
	int sockfd;
	int rc;
	int rhport;
	int vhci_ix;

	sockfd = usbip_net_tcp_connect(host, usbip_port_string);
	if (sockfd < 0) {
		err("tcp connect");
		return -1;
	}

	rc = query_import_device(sockfd, busid, &vhci_ix, &rhport);
	if (rc < 0)
		return -1;

	close(sockfd);

	rc = record_connection(host, usbip_port_string, busid, vhci_ix, rhport);
	if (rc < 0) {
		err("record connection");
		return -1;
	}

	return 0;
}

int usbip_attach(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "remote", required_argument, NULL, 'r' },
		{ "busid",  required_argument, NULL, 'b' },
		{ "device",  required_argument, NULL, 'd' },
		{ NULL, 0,  NULL, 0 }
	};
	char *host = NULL;
	char *busid = NULL;
	int opt;
	int ret = -1;

	for (;;) {
		opt = getopt_long(argc, argv, "d:r:b:", opts, NULL);

		if (opt == -1)
			break;

		switch (opt) {
		case 'r':
			host = optarg;
			break;
		case 'd':
		case 'b':
			busid = optarg;
			break;
		default:
			goto err_out;
		}
	}

	if (!host || !busid)
		goto err_out;

	ret = attach_device(host, busid);
	goto out;

err_out:
	usbip_attach_usage();
out:
	return ret;
}
