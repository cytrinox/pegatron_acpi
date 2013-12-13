/*
 * ACPI-WMI driver for Pegatron platform
 *
 * Copyright (C) 2013 Marco Antonio Benatto <benatto@mandriva.com.br>
 *
 * Development of this program was funded by Philco Informática.
 * Most of this driver was written following the ACPI specification provided
 * by Pegatron Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of GNU General Public License version 2 as
 * published by Free Software Foundation
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/printk.h>
#include <linux/rfkill.h>
#include <linux/input.h>
#include <linux/input/sparse-keymap.h>
#include <linux/platform_device.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>

#define PEGATRON_DEVICE_ID "PTK0001"
#define PEGATRON_FILE KBUILD_MODNAME

#define PEGATRON_WMI_GUID "79142400-C6A3-40FA-BADB-8A2652834100"
#define PEGATRON_WMI_EVENT_GUID "59142400-C6A3-40FA-BADB-8A2652834100"


#define PEGATRON_WLAN_EVENT 88


MODULE_AUTHOR("Marco Antonio Benatto");
MODULE_DESCRIPTION("Pegatron ACPI/WMI platform device driver");
MODULE_LICENSE("GPL");

MODULE_ALIAS("wmi:79142400-C6A3-40FA-BADB-8A2652834100");

static int pegatron_acpi_add(struct acpi_device*);
static int pegatron_acpi_remove(struct acpi_device*);
static void pegatron_acpi_notify(struct acpi_device*, u32);

static const struct acpi_device_id pegatron_device_ids[] = {
	{ PEGATRON_DEVICE_ID, 0 },
	{ "", 0}
};
MODULE_DEVICE_TABLE(acpi, pegatron_device_ids);

/*
 * Pegatron specific hotkeys mapping
 */
static const struct key_entry pegatron_keymap[] = {
	{KE_KEY, 0xf1, {KEY_WLAN} }, /* WLAN on/off hotkey */
	{KE_KEY, 0xf3, {KEY_PROG2} }, /* Smart battery hotkey */
	{KE_KEY, 0xf8, {KEY_TOUCHPAD_TOGGLE} }, /* TouchPad lock hotkey */
	{KE_END, 0},
};


/*
 * Wlan rfkill information
 */
struct pegatron_rfkill_info {
	struct rfkill *rfkill;
	u32 dev_id;
	int status;
};

/*
 * Pegatron laptop information
 */

struct pegatron_laptop {
	struct acpi_device *dev;

	struct input_dev *inputdev; /* Pegatron input device */
	struct key_entry *keymap; /* Pegatron hotkeys sparse keymap */

	acpi_handle handle;	

	struct pegatron_rfkill_info wlan_rfkill; /* wlan rfkill information */

	struct mutex hotplug_lock; 
	struct mutex wmi_lock;
};

/*
 * BIOS argument struct
 */
struct bios_args {
	u32 arg0;
};

/*
 * Pegatron ACPI driver information
 */

static struct acpi_driver pegatron_acpi_driver = {
	.name =			"Pegatron ACPI",
	.class = 		"Pegatron",
	.ids = 			pegatron_device_ids,
	.flags =		ACPI_DRIVER_ALL_NOTIFY_EVENTS,
	.ops =			{
						.add = pegatron_acpi_add,
						.remove = pegatron_acpi_remove,
						.notify = pegatron_acpi_notify,
					},
	.owner =		THIS_MODULE,
};

static struct platform_driver platform_driver = {
		.driver = {
			.name = PEGATRON_FILE,
			.owner = THIS_MODULE,
		},
};


/*
 * init functions
 */

static int pegatron_acpi_init(struct pegatron_laptop *pegatron) {
	acpi_status status = AE_OK;

	if (!acpi_has_method(pegatron->handle, "INIT")){
		dev_err(&pegatron->dev->dev, "[Pegatron] INIT method not found on DSDT\n");
		return -ENODEV;
	}

	status = acpi_execute_simple_method(pegatron->handle, "INIT", 0x55AA66BC);
	if (ACPI_FAILURE(status)){
		dev_err(&pegatron->dev->dev, "[Pegatron] error calling ACPI INIT method\n");
		return -ENODEV;
	}

	status = acpi_execute_simple_method(pegatron->handle, "NTFY", 0x02);
	if (ACPI_FAILURE(status)){
		dev_err(&pegatron->dev->dev, "[Pegatron] error calling ACPI INIT method\n");
		return -ENODEV;
	}

	return 0;
}

static int pegatron_input_init(struct pegatron_laptop *pegatron) {
	struct input_dev *input;
	int failed;

	pr_info("[Pegatron] Initializing input devices\n");

	input = input_allocate_device();

	if (!input){
		pr_warn("[Pegatron] Failed to allocate input device\n");
		return -ENOMEM;
	}

	input->name = "Pegatron laptop extra buttons";
	input->phys = PEGATRON_FILE "/input0";
	input->id.bustype = BUS_HOST;
	
	/*Generic device from Pegatron ACPI device*/
	input->dev.parent = &pegatron->dev->dev; 
	
	/* Setting up sparse keymap */
	failed = sparse_keymap_setup(input, pegatron_keymap, NULL);

	if (failed) {
		pr_warn("[Pegatron] Unable to setup Pegatron keymap\n");
		input_free_device(input);
		return failed;
	}

	failed = input_register_device(input);

	if (failed) {
		pr_warn("[Pegatron] Unable to register Pegatron input\n");
		sparse_keymap_free(input);
		input_free_device(input);
		return failed;
	}

	pegatron->inputdev = input;
	
	return 0;
}

static int pegatron_rfkill_init(struct pegatron_laptop *pegatron) {
	int result = 0;

	mutex_init(&pegatron->hotplug_lock);
	mutex_init(&pegatron->wmi_lock);

	/* TODO: start rfkill and free both mutexes */

}


/*****************************************************************************/



/*
 * exit functions
 */

static void pegatron_input_exit(struct pegatron_laptop *pegatron) {
	if (pegatron->inputdev) {
		sparse_keymap_free(pegatron->inputdev);
		input_unregister_device(pegatron->inputdev);
	}

	pegatron->inputdev = NULL;
}

/*****************************************************************************/


/*
 * MISC functions
 */

static int pegatron_acpi_add(struct acpi_device *dev) {
	struct pegatron_laptop *pegatron;
	int result;

	pegatron = kzalloc(sizeof(struct pegatron_laptop), GFP_KERNEL);

	if (!pegatron){
		dev_err(&dev->dev, "[Pegatron] Error allocating memory for pegatron laptop\n");
		return -ENOMEM;
	}

	pegatron->handle = dev->handle;
	dev->driver_data = pegatron;
	pegatron->dev = dev;

	/* Initializing acpi */
	result = pegatron_acpi_init(pegatron);

	/* If we fail to init something we release allocated memory */
	if (result) {
		kfree(pegatron);
		return result;
	}

	/* Initializing Pegatron specific input device */
	result = pegatron_input_init(pegatron);

	if (result) {
		kfree(pegatron);
		return result;
	}
	

	return result;
}

static int pegatron_query_hotkey(const struct pegatron_laptop* pegatron
				, const int event, unsigned int *scancode) {

	acpi_status status = AE_OK;
	unsigned long long *data = kzalloc(sizeof(unsigned long long int),
									  GFP_KERNEL);

	if (!data) {
		dev_err(&pegatron->dev->dev, "[Pegatron] No memory enough\n");
		return -ENOMEM;
	}

	if (!acpi_has_method(pegatron->handle, "WQ00")) {
		dev_err(&pegatron->dev->dev,
				"[Pegatron] Could not find required function in ACPI table\n");
		kfree(data);
		return -ENODEV;
	}

	status = acpi_evaluate_integer(pegatron->handle, "WQ00", NULL, data);

	if (ACPI_FAILURE(status)){
		dev_err(&pegatron->dev->dev, "[Pegatron] error calling ACPI INIT method\n");
		kfree(data);
		return -ENODEV;
	}

	*scancode = *(data);

	pr_info("[Pegatron] Event scancode: 0x%x\n", *scancode);

	kfree(data);

	return 0;
}

static void pegatron_input_notify(struct pegatron_laptop *pegatron, int event) {
		if (!pegatron->inputdev)
				return;

		switch (event){
			case PEGATRON_WLAN_EVENT:

				break;
			default:
				if (!sparse_keymap_report_event(pegatron->inputdev, event, 1, true)) {
						pr_info("[Pegatron] Unknown key %x pressed\n", event);
				}else{
						pr_info("[Pegatron] to be done in input notify\n");
				}
		}
}

static void pegatron_acpi_notify(struct acpi_device *dev, u32 event) {
		struct pegatron_laptop *pegatron = acpi_driver_data(dev);
		pr_info("[Pegatron] event triggered: %x\n", event);

		pegatron_input_notify(pegatron, event);
}

static void pegatron_notify_handler(u32 value, void *context) {
		struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
		union acpi_object *obj;
		acpi_status status;
		int code;
		unsigned int key_value = 1;
		/*struct pegatron_laptop *laptop = */

		pr_info("[Pegatron] event received\n");

		status = wmi_get_event_data(value, &response);

		if (status != AE_OK) {
			pr_err("[Pegatron] bad event status 0x%x\n", status);
			return;
		}

		obj = (union acpi_object*)response.pointer;

		if (obj && obj->type == ACPI_TYPE_INTEGER) {
			code = obj->integer.value;
			
			pr_info("[Pegatron] received key event: %x\n", code);

		}

		switch (code) {
			case PEGATRON_WLAN_EVENT:
				pr_info("[Pegatron] wlan event received\n");
				break;
			default:
				pr_info("[Pegatron] operation not implemented yet\n");
				break;
		}

		kfree(obj);
}

static int pegatron_acpi_remove(struct acpi_device *dev) {
	struct pegatron_laptop *pegatron = acpi_driver_data(dev);

	pr_info("[Pegatron] removing acpi data\n");
	
	/* TODO: exit all other stuff to be done */

	pegatron_input_exit(pegatron);

	kfree(pegatron);
	
	return 0;
}


static int pegatron_start_wmi(void) {
		struct bios_args args = {
			.arg0 = 0x1,
		};

		struct acpi_buffer input = { (acpi_size) sizeof(args), &args };
		struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
		acpi_status status;

		status = wmi_evaluate_method(PEGATRON_WMI_GUID, 0x01, 0x06, &input, &output);

		if (ACPI_FAILURE(status)) {
			pr_err("[Pegatron] unable to find this method\n");
			return -1;
		}

		return 0;
}

static int pegatron_setup_rfkill(struct pegatron_laptop *pegatron,
								 const char *name, enum rfkill_type type,
								 int dev_id) {
	
}


/*****************************************************************************/


static int __init pegatron_laptop_init(void) {
	int result = 0;
	acpi_status status = AE_OK;

	pr_info("[Pegatron] ACPI/WMI module loaded(GUID: %s)\n", PEGATRON_WMI_GUID);

	result = platform_driver_register(&platform_driver);
	if (result < 0)
			return result;

	result = acpi_bus_register_driver(&pegatron_acpi_driver);
	if (result < 0) {
		pr_err("[Pegatron] Could not insert Pegatron device driver. Exiting...\n");
		platform_driver_unregister(&platform_driver);
		return -ENODEV;
	}

	pr_info("[Pegatron] Checking for WMI GUID information\n");

	if (!wmi_has_guid(PEGATRON_WMI_GUID)) {
		pr_err("[Pegatron] WMI information doesn't match. Exiting...\n");
		return -ENODEV;
	}

	pr_info("[Pegatron] Installing WMI event handler\n");

	status = wmi_install_notify_handler(PEGATRON_WMI_GUID, pegatron_notify_handler, NULL);

	if(ACPI_FAILURE(status)) {
		pr_err("[Pegatron] Error installing notify handler. Exiting...\n");
		return -EIO;
	}

	status = wmi_install_notify_handler(PEGATRON_WMI_EVENT_GUID, pegatron_notify_handler, NULL);
	if(ACPI_FAILURE(status)) {
		pr_err("[Pegatron] Error installing event notify handler. Exiting...\n");
		return -EIO;
	}
	
	pegatron_start_wmi();

	pr_info("[Pegatron] Module initialized successfully\n");
	return 0;
}

static void __exit pegatron_acpi_exit(void) {
	pr_info("[Pegatron] Unloading ACPI/WMI device\n");
	acpi_bus_unregister_driver(&pegatron_acpi_driver);
	platform_driver_unregister(&platform_driver);
	wmi_remove_notify_handler(PEGATRON_WMI_GUID);
	wmi_remove_notify_handler(PEGATRON_WMI_EVENT_GUID);

}

module_init(pegatron_laptop_init);
module_exit(pegatron_acpi_exit);
