// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2003-2008 Takahiro Hirofuchi
 * Copyright (C) 2015-2016 Nobuo Iwata
 */

#include <linux/kthread.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* Hardening for Spectre-v1 */
#include <linux/nospec.h>

#include "usbip_common.h"
#include "vhci.h"

/* TODO: refine locking ?*/

/*
 * output example:
 * hub port sta spd dev       sockfd local_busid
 * hs  0000 004 000 00000000  000003 1-2.3
 * ................................................
 * ss  0008 004 000 00000000  000004 2-3.4
 * ................................................
 *
 * Output includes socket fd instead of socket pointer address to avoid
 * leaking kernel memory address in:
 *	/sys/devices/platform/vhci_hcd.0/status and in debug output.
 * The socket pointer address is not used at the moment and it was made
 * visible as a convenient way to find IP address from socket pointer
 * address by looking up /proc/net/{tcp,tcp6}. As this opens a security
 * hole, the change is made to use sockfd instead.
 *
 */
static void port_show_vhci(char **out, int hub, int port, struct vhci_device *vdev)
{
	if (hub == HUB_SPEED_HIGH)
		*out += sprintf(*out, "hs  %04u %03u ",
				      port, vdev->ud.status);
	else /* hub == HUB_SPEED_SUPER */
		*out += sprintf(*out, "ss  %04u %03u ",
				      port, vdev->ud.status);

	if (vdev->ud.status == VDEV_ST_USED) {
		*out += sprintf(*out, "%03u %08x ",
				      vdev->speed, vdev->devid);
		*out += sprintf(*out, "%06u %s",
				      vdev->ud.sockfd,
				      dev_name(&vdev->udev->dev));

	} else {
		*out += sprintf(*out, "000 00000000 ");
		*out += sprintf(*out, "000000 0-0");
	}

	*out += sprintf(*out, "\n");
}

/* Sysfs entry to show port status */
static ssize_t status_show_vhci(struct vhci *vhci, char *out)
{
	char *s = out;
	int i;
	unsigned long flags;

	if (WARN_ON(!vhci) || WARN_ON(!out))
		return 0;

	spin_lock_irqsave(&vhci->lock, flags);

	for (i = 0; i < VHCI_HC_PORTS; i++) {
		struct vhci_device *vdev = &vhci->vhci_hcd_hs->vdev[i];

		spin_lock(&vdev->ud.lock);
		port_show_vhci(&out, HUB_SPEED_HIGH,
			       i, vdev);
		spin_unlock(&vdev->ud.lock);
	}

	for (i = 0; i < VHCI_HC_PORTS; i++) {
		struct vhci_device *vdev = &vhci->vhci_hcd_ss->vdev[i];

		spin_lock(&vdev->ud.lock);
		port_show_vhci(&out, HUB_SPEED_SUPER,
			       VHCI_HC_PORTS + i, vdev);
		spin_unlock(&vdev->ud.lock);
	}

	spin_unlock_irqrestore(&vhci->lock, flags);

	return out - s;
}

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *out)
{
	char *s = out;
	out += sprintf(out,
		       "hub port sta spd dev      sockfd local_busid\n");
	out += status_show_vhci(device_attribute_to_vhci(attr), out);
	return out - s;
}

static ssize_t nports_show(struct device *dev, struct device_attribute *attr,
			   char *out)
{
	return sprintf(out, "%d\n", VHCI_PORTS);
}

/* Sysfs entry to shutdown a virtual connection */
static int vhci_port_disconnect(struct vhci_hcd *vhci_hcd, __u32 rhport)
{
	struct vhci_device *vdev = &vhci_hcd->vdev[rhport];
	struct vhci *vhci = vhci_hcd->vhci;
	unsigned long flags;

	usbip_dbg_vhci_sysfs("enter\n");

	/* lock */
	spin_lock_irqsave(&vhci->lock, flags);
	spin_lock(&vdev->ud.lock);

	if (vdev->ud.status == VDEV_ST_NULL) {
		pr_err("not connected %d\n", vdev->ud.status);

		/* unlock */
		spin_unlock(&vdev->ud.lock);
		spin_unlock_irqrestore(&vhci->lock, flags);

		return -EINVAL;
	}

	/* unlock */
	spin_unlock(&vdev->ud.lock);
	spin_unlock_irqrestore(&vhci->lock, flags);

	usbip_event_add(&vdev->ud, VDEV_EVENT_DOWN);

	return 0;
}

static int validate_port_in_range(__u32 port, __u32 base, __u32 top)
{
	if (port < base || port >= top) {
		pr_err("Port number %u outside of range [%u-%u]\n",
		       port, base, top - 1);
		return 0;
	}
	return 1;
}

static ssize_t detach_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct vhci *vhci = device_attribute_to_vhci(attr);
	__u32 port = 0;
	int ret;

	if (kstrtoint(buf, 10, &port) < 0)
		return -EINVAL;

	usbip_dbg_vhci_sysfs("%s: detach port %d\n", dev_name(dev), port);

	if (!validate_port_in_range(port, 0, VHCI_PORTS))
		return -EINVAL;

	if (port >= VHCI_HC_PORTS)
		ret = vhci_port_disconnect(vhci->vhci_hcd_ss,
					   port - VHCI_HC_PORTS);
	else
		ret = vhci_port_disconnect(vhci->vhci_hcd_hs, port);

	if (ret < 0)
		return -EINVAL;

	usbip_dbg_vhci_sysfs("Leave\n");

	return count;
}

/* Sysfs entry to establish a virtual connection */
/*
 * To start a new USB/IP attachment, a userland program needs to setup a TCP
 * connection and then write its socket descriptor with remote device
 * information into this sysfs file.
 *
 * A remote device is virtually attached to the root-hub port of @rhport with
 * @speed. @devid is embedded into a request to specify the remote device in a
 * server host.
 *
 * write() returns 0 on success, else negative errno.
 */
static ssize_t attach_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct vhci *vhci = device_attribute_to_vhci(attr);
	struct socket *socket;
	int sockfd = 0;
	__u32 port = 0, devid = 0, speed = 0;
	struct vhci_device *vdev;
	int err;
	unsigned long flags;

	/*
	 * @port: port number of vhci_hcd
	 * @sockfd: socket descriptor of an established TCP connection
	 * @devid: unique device identifier in a remote host
	 * @speed: usb device speed in a remote host
	 */
	if (sscanf(buf, "%u %u %u %u", &port, &sockfd, &devid, &speed) != 4)
		return -EINVAL;

	usbip_dbg_vhci_sysfs("%s: attach port(%u) sockfd(%u) devid(%u) speed(%u)\n",
			     dev_name(dev), port, sockfd, devid, speed);

	/* check received parameters */
	switch (speed) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
	case USB_SPEED_HIGH:
	case USB_SPEED_WIRELESS:
		if (!validate_port_in_range(port, 0, VHCI_HC_PORTS))
			return -EINVAL;
		vdev = &vhci->vhci_hcd_hs->vdev[port];
		break;
	case USB_SPEED_SUPER:
		if (!validate_port_in_range(port, VHCI_HC_PORTS, VHCI_PORTS))
			return -EINVAL;
		vdev = &vhci->vhci_hcd_ss->vdev[port - VHCI_HC_PORTS];
		break;
	default:
		pr_err("Failed attach request for unsupported USB speed: %s\n",
		       usb_speed_string(speed));
		return -EINVAL;
	}

	/* Extract socket from fd. */
	socket = sockfd_lookup(sockfd, &err);
	if (!socket)
		return -EINVAL;

	/* now need lock until setting vdev status as used */

	/* begin a lock */
	spin_lock_irqsave(&vhci->lock, flags);
	spin_lock(&vdev->ud.lock);

	if (vdev->ud.status != VDEV_ST_NULL) {
		/* end of the lock */
		spin_unlock(&vdev->ud.lock);
		spin_unlock_irqrestore(&vhci->lock, flags);

		sockfd_put(socket);

		dev_err(dev, "port %u already used\n", port);
		/*
		 * Will be retried from userspace
		 * if there's another free port.
		 */
		return -EBUSY;
	}

	dev_info(dev, "port(%u) sockfd(%d)\n",
		 port, sockfd);
	dev_info(dev, "devid(%u) speed(%u) speed_str(%s)\n",
		 devid, speed, usb_speed_string(speed));

	vdev->devid         = devid;
	vdev->speed         = speed;
	vdev->ud.sockfd     = sockfd;
	vdev->ud.tcp_socket = socket;
	vdev->ud.status     = VDEV_ST_NOTASSIGNED;

	spin_unlock(&vdev->ud.lock);
	spin_unlock_irqrestore(&vhci->lock, flags);
	/* end the lock */

	vdev->ud.tcp_rx = kthread_get_run(vhci_rx_loop, &vdev->ud, "vhci_rx");
	vdev->ud.tcp_tx = kthread_get_run(vhci_tx_loop, &vdev->ud, "vhci_tx");

	rh_port_connect(vdev, speed);

	return count;
}

int vhci_init_attr_group(struct vhci_hcd *vhci_hcd, int id)
{
	struct vhci_attrs *vhci_attrs;
	struct attribute **attrs;
	struct vhci *vhci = vhci_hcd->vhci;
	int nattrs = ((id == 0) ? 5 : 4);

	if (WARN_ON(vhci->attrs != NULL))
		return -EADDRINUSE;

	vhci_attrs = kcalloc(1, sizeof(*vhci_attrs), GFP_KERNEL);
	if (vhci_attrs == NULL)
		return -ENOMEM;

	attrs = kmalloc_array(nattrs + 1, sizeof(*attrs), GFP_KERNEL);
	if (attrs == NULL) {
		kfree(vhci_attrs);
		return -ENOMEM;
	}

	vhci->attrs = vhci_attrs;
	vhci_attrs->attribute_group.attrs = attrs;

	vhci_attrs->dev_attr_status.attr.attr.name = "status";
	vhci_attrs->dev_attr_status.attr.attr.mode = 0444;
	vhci_attrs->dev_attr_status.attr.show = status_show;
	vhci_attrs->dev_attr_status.var = vhci;
	sysfs_attr_init(&vhci_attrs->dev_attr_status.attr.attr);
	attrs[0] = &vhci_attrs->dev_attr_status.attr.attr;

	vhci_attrs->dev_attr_attach.attr.attr.name = "attach";
	vhci_attrs->dev_attr_attach.attr.attr.mode = 0200;
	vhci_attrs->dev_attr_attach.attr.store = attach_store;
	vhci_attrs->dev_attr_attach.var = vhci;
	sysfs_attr_init(&vhci_attrs->dev_attr_attach.attr.attr);
	attrs[1] = &vhci_attrs->dev_attr_attach.attr.attr;

	vhci_attrs->dev_attr_detach.attr.attr.name = "detach";
	vhci_attrs->dev_attr_detach.attr.attr.mode = 0200;
	vhci_attrs->dev_attr_detach.attr.store = detach_store;
	vhci_attrs->dev_attr_detach.var = vhci;
	sysfs_attr_init(&vhci_attrs->dev_attr_detach.attr.attr);
	attrs[2] = &vhci_attrs->dev_attr_detach.attr.attr;

	vhci_attrs->dev_attr_nports.attr.attr.name = "nports";
	vhci_attrs->dev_attr_nports.attr.attr.mode = 0444;
	vhci_attrs->dev_attr_nports.attr.show = nports_show;
	vhci_attrs->dev_attr_nports.var = vhci;
	sysfs_attr_init(&vhci_attrs->dev_attr_nports.attr.attr);
	attrs[3] = &vhci_attrs->dev_attr_nports.attr.attr;

	if (id == 0) {
		attrs[4] = &dev_attr_usbip_debug.attr;
		attrs[5] = NULL;
	} else {
		attrs[4] = NULL;
	}

	return 0;
}

void vhci_finish_attr_group(struct vhci_hcd *vhci_hcd)
{
	struct vhci_attrs *vhci_attrs = vhci_hcd->vhci->attrs;

	if (vhci_attrs) {
		struct attribute **attrs = vhci_attrs->attribute_group.attrs;

		kfree(attrs);
		kfree(vhci_attrs);
		vhci_hcd->vhci->attrs = NULL;
	}
}
