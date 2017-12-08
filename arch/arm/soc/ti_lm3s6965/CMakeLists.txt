zephyr_sources(
  soc.c
  soc_config.c
  )
zephyr_sources_ifdef(CONFIG_QEMU_ARM_MPU_ENABLE arm_mpu_regions.c)
