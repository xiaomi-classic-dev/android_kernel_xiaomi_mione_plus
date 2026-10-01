/*
 * bootinfo.c
 *
 * Copyright (C) 2011-2014 Xiaomi Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiOne bootloader interface restored from the mione-l branch.
 */

#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/sysfs.h>

#include <asm/bootinfo.h>
#include <asm/page.h>

static const char * const powerup_reasons[PU_REASON_MAX] = {
	[PU_REASON_EVENT_KEYPAD]		= "keypad",
	[PU_REASON_EVENT_RTC]		= "rtc",
	[PU_REASON_EVENT_CABLE]		= "cable",
	[PU_REASON_EVENT_SMPL]		= "smpl",
	[PU_REASON_EVENT_WDOG]		= "wdog",
	[PU_REASON_EVENT_USB_CHG]		= "usb_chg",
	[PU_REASON_EVENT_WALL_CHG]	= "wall_chg",
};

static const char * const reset_reasons[RS_REASON_MAX] = {
	[RS_REASON_EVENT_WDOG]		= "wdog",
	[RS_REASON_EVENT_MPM]		= "mpm pu reset",
	[RS_REASON_EVENT_SECRST]		= "security control power on reset",
	[RS_REASON_EVENT_KPANIC]		= "kpanic",
	[RS_REASON_EVENT_NORMAL]		= "reboot",
	[RS_REASON_EVENT_OTHER]		= "other",
};

static unsigned int powerup_reason;
static struct kobject *bootinfo_kobj;

unsigned int get_powerup_reason(void)
{
	return powerup_reason;
}
EXPORT_SYMBOL(get_powerup_reason);

void set_powerup_reason(unsigned int reason)
{
	powerup_reason = reason;
}
EXPORT_SYMBOL(set_powerup_reason);

int is_abnormal_powerup(void)
{
	return get_powerup_reason() & (RESTART_EVENT_KPANIC |
				      RESTART_EVENT_WDOG | RESTART_EVENT_OTHER);
}

static ssize_t powerup_reason_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	unsigned int reason = get_powerup_reason();
	unsigned int reset_reason = reason >> 16;
	unsigned int index;
	const char *name = "unknown";

	/* Preserve Xiaomi's first-set-bit decoding; keep the raw word too. */
	if (reset_reason) {
		index = __ffs(reset_reason);
		if (index < RS_REASON_MAX) {
			name = reset_reasons[index];
			goto out;
		}
	}
	if (reason) {
		index = __ffs(reason);
		if (index < PU_REASON_MAX)
			name = powerup_reasons[index];
	}
out:
	return scnprintf(buf, PAGE_SIZE, "%s\n", name);
}

static ssize_t powerup_reason_raw_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%08x\n", get_powerup_reason());
}

static struct kobj_attribute powerup_reason_attr = __ATTR_RO(powerup_reason);
static struct kobj_attribute powerup_reason_raw_attr =
	__ATTR_RO(powerup_reason_raw);

static struct attribute *bootinfo_attrs[] = {
	&powerup_reason_attr.attr,
	&powerup_reason_raw_attr.attr,
	NULL,
};

static const struct attribute_group bootinfo_attr_group = {
	.attrs = bootinfo_attrs,
};

static int __init bootinfo_init(void)
{
	int ret;

	bootinfo_kobj = kobject_create_and_add("bootinfo", NULL);
	if (!bootinfo_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(bootinfo_kobj, &bootinfo_attr_group);
	if (ret) {
		pr_err("bootinfo: cannot create attributes: %d\n", ret);
		kobject_put(bootinfo_kobj);
		bootinfo_kobj = NULL;
	}
	return ret;
}
core_initcall(bootinfo_init);
