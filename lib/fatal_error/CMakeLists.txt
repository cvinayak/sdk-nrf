#
# Copyright (c) 2019 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

if(CONFIG_RESET_ON_FATAL_ERROR OR CONFIG_FATAL_ERROR_HANDLERS)
  zephyr_library()
  zephyr_library_sources(fatal_error.c)
  zephyr_iterable_section(NAME fatal_error_handler KVMA RAM_REGION GROUP RODATA_REGION)
endif()
