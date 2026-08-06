.. _lib_fatal_error:

Fatal error handler
###################

.. contents::
   :local:
   :depth: 2

The |NCS| provides an implementation of the fatal error handler that overrides Zephyr's default fatal error handling implementation.

Overview
********

The library defines the :c:func:`k_sys_fatal_error_handler` function, which is declared in Zephyr's :ref:`fatal error handling API <zephyr:fatal>`.
Its implementation in the |NCS| is standardized for most samples.

Before the system is reset, the library runs all the pre-reset handlers registered with the :c:macro:`FATAL_ERROR_HANDLER_DEFINE` macro.
This lets a module report the fatal error over its own transport, without overriding the :c:func:`k_sys_fatal_error_handler` function.
For example, the :ref:`ipc_radio` application uses such a handler to report the fatal error to the application core as a Bluetooth\ :sup:`®` Vendor Specific HCI event.

A pre-reset handler is called from the context in which the fatal error was raised.
This can be a thread, an interrupt or a zero-latency interrupt context, and the interrupts are locked.
The handler must therefore not block, allocate with a timeout or yield.
Zero-latency interrupts are not masked by the interrupt lock, so a handler must also tolerate being preempted by a fatal error raised from a zero-latency interrupt.

Configuration
*************

When building for an embedded target, the default behavior of the :c:func:`k_sys_fatal_error_handler` function in case of a fatal error is to reboot the application.
You can modify the default behavior of the library not to reboot the application.
To have the application enter an endless loop, change the :kconfig:option:`CONFIG_RESET_ON_FATAL_ERROR` Kconfig option to ``n``.

Library files
*************

This library can be found under :file:`lib/fatal_error/` in the |NCS| folder structure.
The Zephyr file it overrides is :file:`fatal.c` under :file:`zephyr/kernel/` in the |NCS| folder structure.

API documentation
*****************

The library is an implementation of Zephyr's :ref:`fatal error handling API <zephyr:fatal>`.
The pre-reset handler registration and the :c:func:`fatal_error_reset` function are declared in the |NCS| header file.

| Header file: :file:`include/fatal_error.h`
| Source file: :file:`lib/fatal_error/fatal_error.c`

.. doxygengroup:: fatal_error
