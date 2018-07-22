/*
 * Copyright (c) 2017-2018, ARM Limited and Contributors. All rights reserved.
 * Copyright (c) 2018, Icenowy Zheng <icenowy@aosc.io>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <arch_helpers.h>
#include <debug.h>

int sunxi_pmic_setup(const char *dt_name)
{
	/* STUB */
	NOTICE("BL31: STUB PMIC setup code for %s\n", dt_name);

	return 0;
}

void __dead2 sunxi_power_down(void)
{
	ERROR("PSCI: Full shutdown not implemented, halting\n");
	wfi();
	panic();
}
