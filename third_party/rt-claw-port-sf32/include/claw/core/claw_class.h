/*
 * SF32 registration shim: use constructors instead of linker sections.
 *
 * Upstream places tool/service/driver registration pointers in custom
 * GNULD sections and expects __start_/__stop_ boundary symbols.  On the
 * SiFli SDK those sections are orphan sections; if they are not inside
 * .data they are not copied from ROM to RAM, and claw_init() faults.
 *
 * To keep the SDK linker scripts untouched and the project self-contained,
 * this shim switches the same registration macros to constructors.  The
 * SDK startup already walks .init_array, so registration happens before
 * claw_init(); the upstream section collectors then see the weak
 * __start_/__stop_ symbols as NULL and skip.
 */

#ifndef CLAW_PORT_CLAW_CLASS_H
#define CLAW_PORT_CLAW_CLASS_H

#include_next <claw/core/claw_class.h>

#undef CLAW_SERVICE_REGISTER
#undef CLAW_DRIVER_REGISTER
#undef CLAW_TOOL_REGISTER

#define CLAW_SERVICE_REGISTER(var_name, svc_ptr)                        \
    __attribute__((constructor))                                        \
    static void __claw_svc_ctor_##var_name(void)                        \
    { claw_service_register((struct claw_service *)(svc_ptr)); }

#define CLAW_DRIVER_REGISTER(var_name, drv_ptr)                         \
    __attribute__((constructor))                                        \
    static void __claw_drv_ctor_##var_name(void)                        \
    { claw_driver_register((struct claw_driver *)(drv_ptr)); }

#define CLAW_TOOL_REGISTER(var_name, tool_ptr)                          \
    __attribute__((constructor))                                        \
    static void __claw_tool_ctor_##var_name(void)                       \
    { claw_tool_core_register((struct claw_tool *)(tool_ptr)); }

#endif /* CLAW_PORT_CLAW_CLASS_H */
