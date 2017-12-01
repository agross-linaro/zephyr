/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef CONFIG_ARM_USERSPACE
        /* Kept in RAM on non-XIP */
#ifdef CONFIG_XIP
	*(".priv_stacks.rodata*")
#endif
#endif /* CONFIG_ARM_USERSPACE */

