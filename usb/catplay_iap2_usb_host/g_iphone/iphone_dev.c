// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/container_of.h>
#include <linux/usb.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/kmod.h>
#include <linux/delay.h>
#include <linux/slab.h>

#include "iphone_dev.h"
#include "iap2_scan.h"

#define IPHONE_ROLE_SWITCH_REBIND_DEBOUNCE_MS 1000
/*
 * Settle time between the head unit acknowledging 0x51 and us dropping the
 * device role. The vendor firmware forces host mode 18ms after the request
 * on a NAC Wave 2; this is the same order of magnitude.
 */
#define IPHONE_ROLE_SWITCH_HOST_DELAY_MS 20
#define IPHONE_RECOVERY_COMMAND "/usr/bin/carlinkit_otalib usboot"
#define IPHONE_RECOVERY_COMMAND_GADGET "/usr/bin/carlinkit_otalib gadget"

static int iphone_dev_set_otg_role(struct g_iphone *iphone_gadget,
					       enum usb_role role);
static int iphone_dev_start_recovery(struct g_iphone *iphone_gadget);
static int iphone_dev_start_gadget(struct g_iphone *iphone_gadget);
static int iphone_dev_set_otg_role_internal(struct iphone_dev_data *data,
					    enum usb_role role,
					    bool manage_gadget_lifecycle);
static void iphone_dev_status_notify_workfn(struct work_struct *work);
static void iphone_dev_role_switch_rebind_workfn(struct work_struct *work);
static void iphone_dev_recovery_workfn(struct work_struct *work);
static const char *iphone_dev_role_switch_name(const char *gadget_name);
static int iphone_dev_update_role_switch_name_from_gadget(struct iphone_dev_data *data);

static int iphone_dev_launch_recovery_process(const char *command)
{
	char *launcher_command;
	char *argv[] = {
		"/bin/sh",
		"-c",
		NULL,
		NULL
	};
	char *envp[] = {
		"HOME=/",
		"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
		"TERM=linux",
		NULL
	};
	int ret;

	launcher_command = kasprintf(GFP_KERNEL,
				      "(%s </dev/null >/dev/null 2>&1 &)",
				      command);
	if (!launcher_command)
		return -ENOMEM;

	argv[2] = launcher_command;

	pr_info("iPhone: launching detached recovery process (fork/disown style)\n");
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
	if (ret)
		pr_err("iPhone: userspace exec failed: %d\n", ret);
	else
		pr_info("iPhone: detached recovery launcher finished\n");

	kfree(launcher_command);
	return ret;
}

static void iphone_dev_recovery_workfn(struct work_struct *work)
{
	struct iphone_dev_data *data =
		container_of(to_delayed_work(work), struct iphone_dev_data,
			     recovery_work);

	pr_info("iPhone: processing deferred recovery request\n");
	iphone_dev_launch_recovery_process(data->recovery_command);
	data->recovery_work_scheduled = false;
}

static void iphone_dev_notify_status_changed(struct g_iphone *iphone_gadget)
{
	struct iphone_dev_data *data =
		container_of(iphone_gadget, struct iphone_dev_data, g);

	schedule_work(&data->status_notify_work);
}

static void iphone_dev_status_notify_workfn(struct work_struct *work)
{
	struct iphone_dev_data *data =
		container_of(work, struct iphone_dev_data, status_notify_work);
	struct device *owner_dev = READ_ONCE(data->owner_dev);

	if (!owner_dev)
		return;

	/*
	 * Pair with g_iphone_set_status() store-release so poll wakeups happen
	 * after the new value is globally visible to sysfs readers.
	 */
	sysfs_notify(&owner_dev->kobj, NULL, "status");
}

static void iphone_dev_remove_accessory_link_locked(struct iphone_dev_data *data)
{
	if (!data->owner_dev || !data->accessory_link_added)
		return;

	sysfs_remove_link(&data->owner_dev->kobj, "iap2_accessory");
	data->accessory_link_added = false;
}

static int iphone_dev_add_accessory_link_locked(struct iphone_dev_data *data,
						struct iap2_acc_accessory *acc)
{
	struct device *acc_dev;
	int ret;

	if (!data->owner_dev || !acc)
		return -ENODEV;

	acc_dev = iap2_acc_device_get(acc);
	if (!acc_dev)
		return -ENODEV;

	iphone_dev_remove_accessory_link_locked(data);
	ret = sysfs_create_link(&data->owner_dev->kobj, &acc_dev->kobj,
				"iap2_accessory");
	put_device(acc_dev);
	if (ret)
		return ret;

	data->accessory_link_added = true;
	return 0;
}

static int iphone_dev_unbind_gadget(struct iphone_dev_data *data)
{
	if (!data->driver_registered)
		return 0;

	pr_info("iPhone: unregistering gadget\n");
	usb_composite_unregister(&data->driver->drv);
	data->driver_registered = false;
	if (data->gadget_registered) {
		pr_warn("iPhone: gadget still marked registered after unregister\n");
		return -EBUSY;
	}

	/* After unbind, always leave USB_ROLE_DEVICE */
	int ret = iphone_dev_set_otg_role_internal(data, USB_ROLE_DEVICE, false);
	return ret;
}

static int iphone_dev_bind_gadget(struct iphone_dev_data *data)
{
	int ret;

	/*
	 * Before bind, always enter USB_ROLE_DEVICE. This must come before the
	 * driver_registered check: the role-switch path goes to host without
	 * unregistering the driver, so after a failed switch the driver is
	 * still registered while the controller sits in host mode. Returning
	 * early here left the dongle in host mode for good, with nothing on
	 * the bus for the head unit to enumerate or retry against.
	 */
	ret = iphone_dev_set_otg_role_internal(data, USB_ROLE_DEVICE, false);
	if (ret) {
		return ret;
	}

	if (data->driver_registered)
		return 0;

	pr_info("iPhone: registering gadget\n");
	ret = c2a_usb_composite_probe(&data->driver->drv);
	if (ret) {
		pr_warn("iPhone: gadget register failed: %d\n", ret);
		return ret;
	}
	data->driver_registered = true;

	return 0;
}

static void iphone_dev_role_switch_rebind_workfn(struct work_struct *work)
{
	struct iphone_dev_data *data =
		container_of(to_delayed_work(work), struct iphone_dev_data,
			     role_switch_rebind_work);
	int ret;

	data->role_switch_rebind_scheduled = false;

	if (g_iphone_get_status(&data->g) != RoleSwitchFailed)
		return;

	pr_info("iPhone: rebinding gadget after role-switch failure debounce\n");
	ret = iphone_dev_bind_gadget(data);
	if (ret) {
		pr_warn("iPhone: debounced gadget rebind failed: %d\n", ret);
		return;
	}

	g_iphone_set_status(&data->g, Initial);
}

static void iphone_dev_clear_accessory_locked(struct iphone_dev_data *data)
{
	struct iap2_acc_accessory *acc = data->acc;

	iphone_dev_remove_accessory_link_locked(data);
	data->acc = NULL;
	if (acc)
		iap2_acc_put_accessory(acc);
}

static void iphone_dev_accessory_watch_work(struct work_struct *work)
{
	struct iphone_dev_data *data =
		container_of(to_delayed_work(work), struct iphone_dev_data,
			     accessory_watch_work);
	struct iap2_acc_accessory *acc;
	bool disconnected = false;

	mutex_lock(&data->lock);
	acc = data->acc;
	if (iap2_acc_is_gone(acc)) {
		disconnected = true;
		iphone_dev_clear_accessory_locked(data);
	}
	mutex_unlock(&data->lock);

	if (!disconnected) {
		schedule_delayed_work(&data->accessory_watch_work,
				      msecs_to_jiffies(100));
		return;
	}

	pr_info("iPhone: accessory disconnected, reverting OTG role\n");
	iphone_dev_set_otg_role(&data->g, USB_ROLE_DEVICE);
	data->g.role_switch_requested = false;
	g_iphone_set_status(&data->g, Initial);
}

static int iphone_dev_set_otg_role(struct g_iphone *iphone_gadget,
				       enum usb_role role)
{
	struct iphone_dev_data *data =
		container_of(iphone_gadget, struct iphone_dev_data, g);
	return iphone_dev_set_otg_role_internal(data, role, true);
}

static int iphone_dev_start_command(struct g_iphone *iphone_gadget,
				    const char *command)
{
	struct iphone_dev_data *data =
		container_of(iphone_gadget, struct iphone_dev_data, g);

	if (data->recovery_work_scheduled) {
		pr_info("iPhone: recovery request ignored (already queued)\n");
		return 0;
	}

	data->recovery_command = command;
	data->recovery_work_scheduled = true;
	pr_info("iPhone: recovery requested, scheduling deferred handler\n");
	if (!schedule_delayed_work(&data->recovery_work, 0)) {
		pr_info("iPhone: recovery request ignored (already scheduled)\n");
		data->recovery_work_scheduled = false;
	}
	return 0;
}

static int iphone_dev_start_recovery(struct g_iphone *iphone_gadget)
{
	return iphone_dev_start_command(iphone_gadget, IPHONE_RECOVERY_COMMAND);
}

static int iphone_dev_start_gadget(struct g_iphone *iphone_gadget)
{
	return iphone_dev_start_command(iphone_gadget,
					IPHONE_RECOVERY_COMMAND_GADGET);
}


static const char *iphone_dev_role_switch_name(const char *gadget_name)
{
	if (!gadget_name || !gadget_name[0])
		return NULL;

	if (!strcmp(gadget_name, "2184200.usb"))
		return "ci_hdrc.1";
	/* V821 exposes MUSB through a child UDC of the USB glue device. */
	if (!strcmp(gadget_name, "musb-hdrc.1.auto"))
		return "44100000.usb";

	return gadget_name;
}

static int iphone_dev_update_role_switch_name_from_gadget(struct iphone_dev_data *data)
{
	const char *gadget_name;
	const char *rs_name;

	if (!data)
		return -ENODEV;

	if (data->udc[0])
		gadget_name = data->udc;
	else if (data->cdev && data->cdev->gadget)
		gadget_name = data->cdev->gadget->name;
	else
		return -ENODEV;

	rs_name = iphone_dev_role_switch_name(gadget_name);
	if (!rs_name)
		return -ENODEV;

	strscpy(data->role_switch_name, rs_name, sizeof(data->role_switch_name));
	return 0;
}

static int set_usb_role(struct iphone_dev_data *data, enum usb_role role)
{
	struct file *role_file;
	char role_path[256];
	const char *role_str;
	const char *rs_name;
	size_t role_len;
	ssize_t written;
	loff_t pos = 0;
	int len;

	if (!data)
		return -EINVAL;

	switch (role) {
	case USB_ROLE_HOST:
		role_str = "host";
		break;
	case USB_ROLE_DEVICE:
		role_str = "device";
		break;
	default:
		return -EINVAL;
	}

	if (iphone_dev_update_role_switch_name_from_gadget(data) &&
	    (!data->role_switch_name[0])) {
		return -ENODEV;
	}
	rs_name = data->role_switch_name;
	role_len = strlen(role_str);

	len = snprintf(role_path, sizeof(role_path),
		       "/sys/class/usb_role/%s-role-switch/role", rs_name);
	if (len < 0 || (size_t)len >= sizeof(role_path))
		return -ENAMETOOLONG;

	role_file = filp_open(role_path, O_WRONLY, 0);
	if (IS_ERR(role_file))
		return PTR_ERR(role_file);

	written = kernel_write(role_file, role_str, role_len, &pos);
	filp_close(role_file, NULL);
	if (written < 0)
		return written;
	if ((size_t)written != role_len)
		return -EIO;

	pr_info("iPhone: setting usb role: %s -> %s\n", role_path, role_str);

	return 0;
}


static int iphone_dev_set_otg_role_internal(struct iphone_dev_data *data,
					    enum usb_role role,
					    bool manage_gadget_lifecycle)
{
	int ret;
	bool role_is_device;

	if (!data || !data->driver)
		return -ENODEV;

	switch (role) {
	case USB_ROLE_DEVICE:
		role_is_device = true;
		break;
	case USB_ROLE_HOST:
		role_is_device = false;
		break;
	default:
		return -EINVAL;
	}

	if (data->otg_role_cache_valid &&
	    data->otg_role_device_cached == role_is_device) {
		pr_debug("iPhone: OTG role '%s' already cached, skipping\n",
			 usb_role_string(role));
		return 0;
	}

	if (role == USB_ROLE_HOST && manage_gadget_lifecycle) {
		ret = iphone_dev_unbind_gadget(data);
		if (ret)
			return ret;
	}

	ret = set_usb_role(data, role);
	if (ret) {
		pr_warn("iPhone: failed OTG override hack: %d\n", ret);
	} else {
		data->otg_role_device_cached = role_is_device;
		data->otg_role_cache_valid = true;
	}

	pr_info("iPhone: set OTG role '%s'\n", usb_role_string(role));

	if (role == USB_ROLE_DEVICE && manage_gadget_lifecycle) {
		ret = iphone_dev_bind_gadget(data);
		if (ret)
			return ret;
	}

	return 0;
}

static void iphone_dev_role_switch_work(struct work_struct *work)
{
	struct iphone_dev_data *data =
		container_of(work, struct iphone_dev_data, role_switch_work);
	struct iap2_acc_accessory *acc;

	if (g_iphone_get_status(&data->g) != RoleSwitch) {
		data->role_switch_work_scheduled = false;
		return;
	}

	if (data->role_switch_rebind_scheduled) {
		cancel_delayed_work_sync(&data->role_switch_rebind_work);
		data->role_switch_rebind_scheduled = false;
	}

	msleep(IPHONE_ROLE_SWITCH_HOST_DELAY_MS);

	if (iphone_dev_set_otg_role_internal(data, USB_ROLE_HOST, false)) {
	// if (iphone_dev_set_otg_role(&data->g, USB_ROLE_HOST)) {
		pr_warn("iPhone: role-switch host transition failed\n");
		data->g.role_switch_requested = false;
		g_iphone_set_status(&data->g, Initial);
		data->role_switch_work_scheduled = false;
		return;
	}

	acc = iap2_acc_probe_accessory();
	if (IS_ERR(acc)) {
		pr_warn("iPhone: role-switch accessory probe failed: %ld\n",
			PTR_ERR(acc));
		/* Go back from host to device, but don't instantly publish the gadget yet; that will happen after debounce */
		/*ret = iphone_dev_set_otg_role_internal(data, USB_ROLE_DEVICE, false);
		if (ret) {
			pr_warn("iPhone: role-switch device rollback failed: %d\n",
				ret);
			data->g.role_switch_requested = false;
			g_iphone_set_status(&data->g, Initial);
			data->role_switch_work_scheduled = false;
			return;
		}*/

		data->g.role_switch_requested = false;
		g_iphone_set_status(&data->g, RoleSwitchFailed);
		msleep(1000); // TODO [hack]
		data->role_switch_rebind_scheduled = true;
		mod_delayed_work(system_wq, &data->role_switch_rebind_work,
				 msecs_to_jiffies(IPHONE_ROLE_SWITCH_REBIND_DEBOUNCE_MS));
		data->role_switch_work_scheduled = false;
		return;
	}

	pr_info("iPhone: role-switch accessory probe succeeded for if=%s\n",
		acc->ifname);

	mutex_lock(&data->lock);
	iphone_dev_clear_accessory_locked(data);
	data->acc = acc;
	if (iphone_dev_add_accessory_link_locked(data, acc))
		pr_warn("iPhone: failed to create accessory iap2_accessory link\n");
	mutex_unlock(&data->lock);

	g_iphone_set_status(&data->g, Accessory);
	mod_delayed_work(system_wq, &data->accessory_watch_work,
			 msecs_to_jiffies(100));
	data->role_switch_work_scheduled = false;
}

static int iphone_dev_start_role_switch_probe(struct g_iphone *iphone_gadget)
{
	struct iphone_dev_data *data =
		container_of(iphone_gadget, struct iphone_dev_data, g);

	if (data->role_switch_work_scheduled)
		return 0;

	data->role_switch_work_scheduled = true;
	schedule_work(&data->role_switch_work);
	return 0;
}

static int iphone_dev_driver_bind(struct usb_composite_dev *cdev)
{
	struct iphone_dev_driver *ipdrv =
		container_of(cdev->driver, struct iphone_dev_driver, drv);
	struct iphone_dev_data *data = ipdrv->data;
	data->cdev = cdev;
	data->gadget_registered = true;
	iphone_dev_update_role_switch_name_from_gadget(data);

	pr_info("iPhone: driver binding 4 configurations\n");

	int ret;

	for (int i = 0; i < 4; i++)
	{
		ret = usb_add_config(cdev, &data->usb_configs[i].cfg, iphone_do_config);
		if (ret)
			return ret;
	}

	return 0;
}

static int iphone_dev_driver_unbind(struct usb_composite_dev *cdev)
{
	struct iphone_dev_driver *ipdrv =
		container_of(cdev->driver, struct iphone_dev_driver, drv);
	struct iphone_dev_data *data = ipdrv->data;

	pr_info("iPhone: driver unbind");

	data->role_switch_work_scheduled = false;
	mutex_lock(&data->lock);
	iphone_dev_clear_accessory_locked(data);
	mutex_unlock(&data->lock);
	data->cdev = NULL;
	data->gadget_registered = false;
	
	return 0;
}

struct iphone_dev_data *iphone_dev_alloc(struct device *owner_dev, char *udc_name, char* serial)
{
	int ret = -ENOMEM;
	struct iphone_dev_data *data;
	size_t count, i;

	if (!serial)
		serial = DEFAULT_IPHONE_SERIAL;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		goto fail;

	/* copy device descriptor */
	data->dev_desc = iphone_device_desc;
	data->owner_dev = owner_dev;
	mutex_init(&data->lock);

	/* copy strings table */
	count = ARRAY_SIZE(iphone_strings);
	data->stringtab = kmemdup(iphone_strings,
	                           sizeof(iphone_strings),
	                           GFP_KERNEL);
	if (!data->stringtab)
		goto fail;

	for (i = 0; data->stringtab[i].s; i++) {
		if (data->stringtab[i].id == 3)
			data->stringtab[i].s = data->serial;
		else if (data->stringtab[i].id == 4)
			data->stringtab[i].s = data->serial_r;
	}

	/* copy serial and serial_r */
	strscpy(data->g.serial, serial, sizeof(data->g.serial));
	strscpy(data->serial, serial, sizeof(data->serial));
	snprintf(data->serial_r, sizeof(data->serial_r), "%s-R", serial);

	/* init gadget_strings */
	data->gadget_strings.language = 0x0409;
	data->gadget_strings.strings  = data->stringtab;

	/* init gadget_strings_array */
	data->gadget_strings_array[0] = &data->gadget_strings;
	data->gadget_strings_array[1] = NULL;

	/* init usb_composite_driver */
	data->driver = kzalloc(sizeof(*data->driver), GFP_KERNEL);
	if (!data->driver)
		goto fail;

	data->driver->drv = (struct usb_composite_driver) {
		.name       = data->driver_name,
		.dev        = &data->dev_desc,
		.strings    = data->gadget_strings_array,
		.max_speed  = USB_SPEED_SUPER,
		.bind       = iphone_dev_driver_bind,
		.unbind     = iphone_dev_driver_unbind,
		.udc_name   = data->udc_name_vec,
	};
	data->driver->data = data;
	data->udc_name_vec[0] = data->udc;
	data->udc_name_vec[1] = NULL;

	/* copy UDC name if provided */
	if (!udc_name) {
		data->driver->drv.udc_name = NULL;
		data->udc_auto = true;
	} else {
		const char *rs_name;

		strscpy(data->udc, udc_name, sizeof(data->udc));
		rs_name = iphone_dev_role_switch_name(udc_name);
		if (rs_name)
			strscpy(data->role_switch_name, rs_name,
				sizeof(data->role_switch_name));
	}

	/* create runtime-unique driver name */
	if (!udc_name) {
		snprintf(data->driver_name, sizeof(data->driver_name), "iphone");
	} else {
		snprintf(data->driver_name, sizeof(data->driver_name), "iphone-%s", udc_name);
	}

	/* init usb configs */
	for (int i = 0; i < ARRAY_SIZE(iphone_configs) && i < 4; i++) {
		data->usb_configs[i].g = &data->g;
		data->usb_configs[i].cfg = iphone_configs[i];
	}

	data->driver_registered = false;
	data->gadget_registered = false;
	data->otg_role_device_cached = true;
	data->otg_role_cache_valid = false;
	data->g.set_otg_role = iphone_dev_set_otg_role;
	data->g.start_role_switch_probe = iphone_dev_start_role_switch_probe;
	data->g.start_recovery = iphone_dev_start_recovery;
	data->g.start_gadget = iphone_dev_start_gadget;
	data->g.notify_status_changed = iphone_dev_notify_status_changed;
	INIT_WORK(&data->status_notify_work, iphone_dev_status_notify_workfn);
	INIT_WORK(&data->role_switch_work, iphone_dev_role_switch_work);
	INIT_DELAYED_WORK(&data->recovery_work, iphone_dev_recovery_workfn);
	INIT_DELAYED_WORK(&data->role_switch_rebind_work,
			  iphone_dev_role_switch_rebind_workfn);
	INIT_DELAYED_WORK(&data->accessory_watch_work,
			  iphone_dev_accessory_watch_work);

	pr_info("iPhone: initial gadget bind with udc_name '%s' rs_name '%s'\n",
		udc_name, data->role_switch_name[0] ? data->role_switch_name : "<unset>");

	ret = iphone_dev_bind_gadget(data);
	if (ret) {
		pr_warn("iPhone: initial gadget bind failed: %d\n", ret);
		goto fail;
	}

	pr_debug("iPhone: initial gadget bind complete\n");
	return data;

fail:
	kfree(data->driver);
	kfree(data->stringtab);
	kfree(data);
	return ERR_PTR(ret);
}

static void iphone_dev_stop_runtime(struct iphone_dev_data *data)
{
	if (!data)
		return;

	cancel_delayed_work_sync(&data->accessory_watch_work);
	cancel_delayed_work_sync(&data->role_switch_rebind_work);
	cancel_delayed_work_sync(&data->recovery_work);
	cancel_work_sync(&data->role_switch_work);
	cancel_work_sync(&data->status_notify_work);
	data->role_switch_work_scheduled = false;
	data->role_switch_rebind_scheduled = false;
	data->recovery_work_scheduled = false;

	mutex_lock(&data->lock);
	iphone_dev_clear_accessory_locked(data);
	mutex_unlock(&data->lock);
}

int iphone_dev_free(struct iphone_dev_data *data) {
	if (!data || !data->driver) {
		return -ENODEV;
	}

	iphone_dev_stop_runtime(data);

	pr_debug("iPhone: calling gadget unregister\n");
	iphone_dev_unbind_gadget(data);
	cancel_work_sync(&data->status_notify_work);
	
	/* This code has been checked several times to verify there is no potential for UAF */
	kfree(data->stringtab);
	kfree(data->driver);
	kfree(data);
	return 0;
}
