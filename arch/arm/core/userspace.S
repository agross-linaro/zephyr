/*
 * Userspace and service handler hooks
 *
 * Copyright (c) 2017 Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include <offsets_short.h>
#include <toolchain.h>
#include <linker/sections.h>
#include <kernel_structs.h>
#include <arch/cpu.h>
#include <syscall.h>

_ASM_FILE_PROLOGUE

GTEXT(_arm_userspace_enter)
GTEXT(_arm_do_syscall)
GDATA(_kernel)

/* Imports */
GTEXT(_k_syscall_table)

/**
 *
 * User space entry function
 *
 * This function is the entry point to user mode from privileged execution.
 * The conversion is one way, and threads which transition to user mode do
 * not transition back later, unless they are doing system calls.
 *
 */
SECTION_FUNC(TEXT,_arm_userspace_enter)
    /* preserve arguments */
#    mov lr, r0

    /*
     * calculate end of stack and set SP to it
     */
    ldr ip, [sp]

    /* push args to stack and save sp in r4 */
    push {r0,r1,r2,r3,r4,r5}
    mov r4, sp

    /* set stack to privileged part of stack */
    add ip, #64
    msr PSP, ip

    /* clear the stack area to clean out privileged data */
    /* from right past the guard right up to the args */
    mov r0, ip
#ifdef CONFIG_INIT_STACKS
    ldr r1,=0xaaaaaaaa
#else
    ldr r1,=0x0
#endif
    /* adjust size to stack size - priv stack - pushed regs */
    sub r2, r4, ip
    bl memset

    msr PSP, r4

    /* setup arguments to thread_entry function */
    ldr r0, =_kernel
    ldr r0, [r0, #_kernel_offset_to_current]
    bl configure_mpu_mem_domain

    /* setup arguments to thread_entry function */
    ldr r0, =_kernel
    ldr r0, [r0, #_kernel_offset_to_current]
    bl configure_mpu_user_context

    pop {r0,r1,r2,r3,r4,r5}
    /* change processor mode to unprivileged */
    mrs ip, CONTROL
    orrs ip, ip, #1
    msr CONTROL, ip

    /* jump to _thread_entry entry */
    ldr ip, =_thread_entry
    bx ip

/**
 *
 * Userspace system call function
 *
 * This function is used to do system calls from unprivileged code.  This
 * function is responsible for the following:
 * 1) Fixing up bad syscalls
 * 2) Configuring privileged stack and loading up stack arguments
 * 3) Dispatching the system call
 * 4) Restoring stack and calling back to the caller of the SVC
 *
 */
SECTION_FUNC(TEXT, _arm_do_syscall)
    /*
     * r0-r3 are values from pre-SVC from stack frame stored during SVC
     * 16 bytes of storage reside on the stack:
     * arg5, arg6, call_id, and LR from SVC frame
     */
    push {r4,r5,r6,lr}

    ldr ip, =_k_syscall_table
    ldr r4, [sp, #24]	/* load call_id from stack */
    lsl r4, #2
    add ip, r4
    ldr ip, [ip]	/* load table address */
    ldr r5, =_SYSCALL_BAD
    lsl r5, #2		/* shift to match the shift we did on the call_id */
    cmp r4, r5
    bne valid_syscall

    /* BAD SYSCALL path */
    /* fixup stack frame on unprivileged stack, adding ssf */
    /* pop registers and lr as this is a one way jump */
    mov r4, sp
    str r4, [sp, #24]
    pop {r4,r5,r6,lr}
    b dispatch_syscall

valid_syscall:
    /* setup priviliged stack */
    ldr r4, =_kernel
    ldr r4, [r4, #_kernel_offset_to_current]
    ldr r5, [r4, #_thread_offset_to_priv_stack_start]    /* priv stack ptr */
    ldr r6, =CONFIG_PRIVILEGED_STACK_SIZE
    add r5, r6

    /* setup privileged stack frame */
    /* 16 bytes: arg5, arg6, ssf, 4 bytes padding */
    sub r5, #16
    ldr r6, [sp, #16]
    str r6, [r5, #0]
    ldr r6, [sp, #20]
    str r6, [r5, #4]
    mov r6, sp
    str r6, [r5, #8]	/* store ssf of unprivileged stack */
    ldr r6, =0
    str r6, [r5, #12]	/* store ssf of unprivileged stack */

    /* switch to privileged stack */
    msr PSP, r5
dispatch_syscall:
    /* execute function from dispatch table */
    blx ip

    /* set stack back to unprivileged stack */
    ldr ip, [sp,#8]
    msr PSP, ip

    pop {r4,r5,r6,lr}

    /* drop privileges by setting bit 0 in CONTROL */
    mrs ip, CONTROL
    orrs ip, ip, #1
    msr CONTROL, ip

    /*
     * return back to original function that called SVC, add 1 to force thumb
     * mode
     */
    ldr ip, [sp, #12]
    orrs ip, ip, #1
    bx ip
