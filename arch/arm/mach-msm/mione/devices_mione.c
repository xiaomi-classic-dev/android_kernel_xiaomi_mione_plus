/* Copyright (c) 2012, LGE Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * MiOne persistent RAM layout ported from cm-12.1 commit 5ce9e6ceae1d.
 */

#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/persistent_ram.h>
#include <linux/platform_device.h>
#include <linux/platform_data/ram_console.h>
#include <linux/string.h>

#include <asm/setup.h>
#include <asm/sizes.h>
#ifdef CONFIG_BOOT_INFO
#include <asm/bootinfo.h>
#endif
#include <mach/board_mione.h>
#include <mach/mione_power_diag.h>

#ifdef CONFIG_ANDROID_RAM_CONSOLE
#define MIONE_PERSISTENT_RAM_SIZE	SZ_1M
#define MIONE_RAM_CONSOLE_SIZE		(248 * SZ_1K)

static struct persistent_ram_descriptor pram_descs[] = {
	{
		.name = "ram_console",
		.size = MIONE_RAM_CONSOLE_SIZE,
	},
};

static struct persistent_ram mione_persistent_ram = {
	.size = MIONE_PERSISTENT_RAM_SIZE,
	.num_descs = ARRAY_SIZE(pram_descs),
	.descs = pram_descs,
};

static bool ram_console_reserved __initdata;

void __init mione_reserve(void)
{
	struct persistent_ram *pram = &mione_persistent_ram;
	struct membank *bank = &meminfo.bank[0];
	int ret;

	if (!meminfo.nr_banks || bank->size < pram->size) {
		pr_err("mione: no memory bank for RAM console\n");
		return;
	}

	/* Keep the same first-bank tail across warm reboots as cm-12.1. */
	pram->start = bank->start + bank->size - pram->size;
	if (!memblock_is_region_memory(pram->start, pram->size) ||
	    memblock_is_region_reserved(pram->start, pram->size)) {
		pr_err("mione: RAM console region is unavailable\n");
		return;
	}

	ret = persistent_ram_early_init(pram);
	if (ret) {
		pr_err("mione: cannot reserve RAM console: %d\n", ret);
		return;
	}
	ram_console_reserved = true;
	/* Console uses 248 KiB; diagnostics use 16 KiB at +256 KiB. */
	BUILD_BUG_ON(MIONE_RAM_CONSOLE_SIZE > SZ_256K);
	BUILD_BUG_ON(SZ_256K + SZ_16K > MIONE_PERSISTENT_RAM_SIZE);
	mione_power_diag_reserve(pram->start + SZ_256K);
}

static char bootreason[256];

static int __init mione_boot_reason(char *s)
{
	if (*s == '=')
		s++;
	/* Leave space for the bootloader ATAG reason below. */
	scnprintf(bootreason, 128,
		  "Boot info:\nLast boot reason: %s\n", s);
	return 1;
}
__setup("bootreason", mione_boot_reason);

static struct ram_console_platform_data ram_console_pdata = {
	.bootinfo = bootreason,
};

static struct platform_device ram_console_device = {
	.name = "ram_console",
	.id = -1,
	.dev = {
		.platform_data = &ram_console_pdata,
	},
};

void __init mione_add_ramconsole_devices(void)
{
	int ret;
#ifdef CONFIG_BOOT_INFO
	size_t len;
#endif

	if (!ram_console_reserved)
		return;

#ifdef CONFIG_BOOT_INFO
	/* This reason describes the reset into the current boot. */
	len = strlen(bootreason);
	scnprintf(bootreason + len, sizeof(bootreason) - len,
		  "%sPowerup reason (ATAG): 0x%08x\n",
		  len ? "" : "Boot info:\n", get_powerup_reason());
#endif

	ret = platform_device_register(&ram_console_device);
	if (ret)
		pr_err("mione: cannot register RAM console: %d\n", ret);
}
#endif /* CONFIG_ANDROID_RAM_CONSOLE */
