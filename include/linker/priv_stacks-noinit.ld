#ifdef CONFIG_ARM_USERSPACE
	/* Put privileged stacks here.
	 */
	. = ALIGN(CONFIG_PRIVILEGED_STACK_SIZE);
	*(".priv_stacks.noinit*")
#endif /* CONFIG_ARM_USERSPACE */
