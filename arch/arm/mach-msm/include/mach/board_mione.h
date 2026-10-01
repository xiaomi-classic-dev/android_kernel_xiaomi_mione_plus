/*
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2008-2012, The Linux Foundation. All rights reserved.
 * Copyright (c) 2012, LGE Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */

#ifndef __ASM_ARCH_MSM_BOARD_MIONE_H
#define __ASM_ARCH_MSM_BOARD_MIONE_H

#include <linux/init.h>

#if defined(CONFIG_MACH_MIONE) && defined(CONFIG_ANDROID_RAM_CONSOLE)
void __init mione_reserve(void);
void __init mione_add_ramconsole_devices(void);
#else
static inline void __init mione_reserve(void) { }
static inline void __init mione_add_ramconsole_devices(void) { }
#endif

#endif /* __ASM_ARCH_MSM_BOARD_MIONE_H */
