/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <misc/printk.h>

static K_THREAD_STACK_DEFINE(stack0, 1024);
static K_THREAD_STACK_DEFINE(stack1, 1024);
static struct k_thread __kernel thread0;
static struct k_thread __kernel thread1;

void print_stuff(void *id, void * unused1, void *unused2)
{
	ARG_UNUSED(id);
	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);

	while (1) {
		printk("%d: foo\n", (u32_t) id);
		k_sleep(1000);
	}
}

void main(void)
{
	k_thread_create(&thread0, stack0, 1024, print_stuff, (void *)0,
			NULL, NULL, 5, K_USER, K_FOREVER);
	k_thread_create(&thread1, stack1, 1024, print_stuff, (void *)1,
			NULL, NULL, 5, K_USER, K_FOREVER);

	k_thread_start(&thread0);
	k_thread_start(&thread1);
}
