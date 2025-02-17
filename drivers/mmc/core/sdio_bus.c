/*
 *  linux/drivers/mmc/core/sdio_bus.c
 *
 *  Copyright 2007 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * SDIO function driver model
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/pm_runtime.h>

#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>

#include "sdio_cis.h"
#include "sdio_bus.h"

#ifdef CONFIG_MMC_EMBEDDED_SDIO
#include <linux/mmc/host.h>
#endif

/* show configuration fields */
#define sdio_config_attr(field, format_string)				\
static ssize_t								\
field##_show(struct device *dev, struct device_attribute *attr, char *buf)				\
{									\
	struct sdio_func *func;						\
									\
	func = dev_to_sdio_func (dev);					\
	return sprintf (buf, format_string, func->field);		\
}

sdio_config_attr(class, "0x%02x\n");
sdio_config_attr(vendor, "0x%04x\n");
sdio_config_attr(device, "0x%04x\n");

static ssize_t modalias_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sdio_func *func = dev_to_sdio_func (dev);

	return sprintf(buf, "sdio:c%02Xv%04Xd%04X\n",
			func->class, func->vendor, func->device);
}

static struct device_attribute sdio_dev_attrs[] = {
	__ATTR_RO(class),
	__ATTR_RO(vendor),
	__ATTR_RO(device),
	__ATTR_RO(modalias),
	__ATTR_NULL,
};

static const struct sdio_device_id *sdio_match_one(struct sdio_func *func,
	const struct sdio_device_id *id)
{
	if (id->class != (__u8)SDIO_ANY_ID && id->class != func->class)
		return NULL;
	if (id->vendor != (__u16)SDIO_ANY_ID && id->vendor != func->vendor)
		return NULL;
	if (id->device != (__u16)SDIO_ANY_ID && id->device != func->device)
		return NULL;
	return id;
}

static const struct sdio_device_id *sdio_match_device(struct sdio_func *func,
	struct sdio_driver *sdrv)
{
	const struct sdio_device_id *ids;

	ids = sdrv->id_table;

	if (ids) {
		while (ids->class || ids->vendor || ids->device) {
			if (sdio_match_one(func, ids))
				return ids;
			ids++;
		}
	}

	return NULL;
}

static int sdio_bus_match(struct device *dev, struct device_driver *drv)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	struct sdio_driver *sdrv = to_sdio_driver(drv);

	if (sdio_match_device(func, sdrv))
		return 1;

	return 0;
}

static int
sdio_bus_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct sdio_func *func = dev_to_sdio_func(dev);

	if (add_uevent_var(env,
			"SDIO_CLASS=%02X", func->class))
		return -ENOMEM;

	if (add_uevent_var(env, 
			"SDIO_ID=%04X:%04X", func->vendor, func->device))
		return -ENOMEM;

	if (add_uevent_var(env,
			"MODALIAS=sdio:c%02Xv%04Xd%04X",
			func->class, func->vendor, func->device))
		return -ENOMEM;

	return 0;
}

static int sdio_bus_probe(struct device *dev)
{
	struct sdio_driver *drv = to_sdio_driver(dev->driver);
	struct sdio_func *func = dev_to_sdio_func(dev);
	const struct sdio_device_id *id;
	int ret;

	id = sdio_match_device(func, drv);
	if (!id)
		return -ENODEV;

	/* Unbound SDIO functions are always suspended.
	 * During probe, the function is set active and the usage count
	 * is incremented.  If the driver supports runtime PM,
	 * it should call pm_runtime_put_noidle() in its probe routine and
	 * pm_runtime_get_noresume() in its remove routine.
	 */
	if (func->card->host->caps & MMC_CAP_POWER_OFF_CARD) {
		ret = pm_runtime_get_sync(dev);
		if (ret < 0)
			goto out;
	}

	if (func->card->host->ios.power_mode == MMC_POWER_OFF) {
		mmc_reinit_host(func->card->host);
	}

	/* Set the default block size so the driver is sure it's something
	 * sensible. */
	sdio_claim_host(func);
	ret = sdio_set_block_size(func, 0);
	sdio_release_host(func);
	if (ret)
		goto disable_runtimepm;

	ret = drv->probe(func, id);
	if (ret)
		goto disable_runtimepm;

	return 0;

disable_runtimepm:
	if (func->card->host->caps & MMC_CAP_POWER_OFF_CARD)
		pm_runtime_put_noidle(dev);
out:
	return ret;
}

static int sdio_bus_remove(struct device *dev)
{
	struct sdio_driver *drv = to_sdio_driver(dev->driver);
	struct sdio_func *func = dev_to_sdio_func(dev);
	int ret = 0;

	/* Make sure card is powered before invoking ->remove() */
	if (func->card->host->caps & MMC_CAP_POWER_OFF_CARD) {
		ret = pm_runtime_get_sync(dev);
		if (ret < 0)
			goto out;
	}

	drv->remove(func);

	if (func->irq_handler) {
		printk(KERN_WARNING "WARNING: driver %s did not remove "
			"its interrupt handler!\n", drv->name);
		sdio_claim_host(func);
		sdio_release_irq(func);
		sdio_release_host(func);
	}

	/* First, undo the increment made directly above */
	if (func->card->host->caps & MMC_CAP_POWER_OFF_CARD)
		pm_runtime_put_noidle(dev);

	/* Then undo the runtime PM settings in sdio_bus_probe() */
	if (func->card->host->caps & MMC_CAP_POWER_OFF_CARD)
		pm_runtime_put_noidle(dev);

out:
	return ret;
}

#ifdef CONFIG_PM_RUNTIME

static int sdio_bus_pm_prepare(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);

	/*
	 * Resume an SDIO device which was suspended at run time at this
	 * point, in order to allow standard SDIO suspend/resume paths
	 * to keep working as usual.
	 *
	 * Ultimately, the SDIO driver itself will decide (in its
	 * suspend handler, or lack thereof) whether the card should be
	 * removed or kept, and if kept, at what power state.
	 *
	 * At this point, PM core have increased our use count, so it's
	 * safe to directly resume the device. After system is resumed
	 * again, PM core will drop back its runtime PM use count, and if
	 * needed device will be suspended again.
	 *
	 * The end result is guaranteed to be a power state that is
	 * coherent with the device's runtime PM use count.
	 *
	 * The return value of pm_runtime_resume is deliberately unchecked
	 * since there is little point in failing system suspend if a
	 * device can't be resumed.
	 */
	if (func->card->host->caps & MMC_CAP_POWER_OFF_CARD)
		pm_runtime_resume(dev);

	return 0;
}

static const struct dev_pm_ops sdio_bus_pm_ops = {
	SET_RUNTIME_PM_OPS(
		pm_generic_runtime_suspend,
		pm_generic_runtime_resume,
		pm_generic_runtime_idle
	)
	.prepare = sdio_bus_pm_prepare,
};

#define SDIO_PM_OPS_PTR	(&sdio_bus_pm_ops)

#else /* !CONFIG_PM_RUNTIME */

#define SDIO_PM_OPS_PTR	NULL

#endif /* !CONFIG_PM_RUNTIME */

static struct bus_type sdio_bus_type = {
	.name		= "sdio",
	.dev_attrs	= sdio_dev_attrs,
	.match		= sdio_bus_match,
	.uevent		= sdio_bus_uevent,
	.probe		= sdio_bus_probe,
	.remove		= sdio_bus_remove,
	.pm		= SDIO_PM_OPS_PTR,
};

int sdio_register_bus(void)
{
	return bus_register(&sdio_bus_type);
}

void sdio_unregister_bus(void)
{
	bus_unregister(&sdio_bus_type);
}

/**
 *	sdio_register_driver - register a function driver
 *	@drv: SDIO function driver
 */
int sdio_register_driver(struct sdio_driver *drv)
{
	drv->drv.name = drv->name;
	drv->drv.bus = &sdio_bus_type;
	return driver_register(&drv->drv);
}
EXPORT_SYMBOL_GPL(sdio_register_driver);

/**
 *	sdio_unregister_driver - unregister a function driver
 *	@drv: SDIO function driver
 */
void sdio_unregister_driver(struct sdio_driver *drv)
{
	drv->drv.bus = &sdio_bus_type;
	driver_unregister(&drv->drv);
}
EXPORT_SYMBOL_GPL(sdio_unregister_driver);

static void sdio_release_func(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);

#ifdef CONFIG_MMC_EMBEDDED_SDIO
	/*
	 * If this device is embedded then we never allocated
	 * cis tables for this func
	 */
	if (!func->card->host->embedded_sdio_data.funcs)
#endif
		sdio_free_func_cis(func);

	if (func->info)
		kfree(func->info);

	kfree(func);
}

/*
 * Allocate and initialise a new SDIO function structure.
 */
struct sdio_func *sdio_alloc_func(struct mmc_card *card)
{
	struct sdio_func *func;

	func = kzalloc(sizeof(struct sdio_func), GFP_KERNEL);
	if (!func)
		return ERR_PTR(-ENOMEM);

	func->card = card;

	device_initialize(&func->dev);

	func->dev.parent = &card->dev;
	func->dev.bus = &sdio_bus_type;
	func->dev.release = sdio_release_func;

	return func;
}

/*
 * Register a new SDIO function with the driver model.
 */
int sdio_add_func(struct sdio_func *func)
{
	int ret;

	dev_set_name(&func->dev, "%s:%d", mmc_card_id(func->card), func->num);

	ret = device_add(&func->dev);
	if (ret == 0)
		sdio_func_set_present(func);

	return ret;
}

/*
 * Unregister a SDIO function with the driver model, and
 * (eventually) free it.
 * This function can be called through error paths where sdio_add_func() was
 * never executed (because a failure occurred at an earlier point).
 */
void sdio_remove_func(struct sdio_func *func)
{
	if (!sdio_func_present(func))
		return;

	device_del(&func->dev);
	put_device(&func->dev);
}

