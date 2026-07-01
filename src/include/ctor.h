/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */
#ifndef _CTOR_H
#define _CTOR_H 1

#define NIOVA_CONSTRUCTOR(priority) \
    __attribute__ ((constructor (priority)))

#define NIOVA_DESTRUCTOR(priority) \
    __attribute__ ((destructor (priority)))

/* -- constructor_priorities --
 * Startup (and shutdown) order for NIOVA subsystems.
 */
enum constructor_priorities {
    INIT_START_CTOR_PRIORITY = 102,
    BACKTRACE_SUBSYS_CTOR_PRIORITY,
    ENV_VAR_SUBSYS_CTOR_PRIORITY,
    WATCHDOG_SUBSYS_CTOR_PRIORITY,
    LREG_SUBSYS_CTOR_PRIORITY,
    FAULT_INJECT_SETUP_CTOR_PRIORITY,
    FAULT_INJECT_CTOR_PRIORITY,
    LOG_SUBSYS_CTOR_PRIORITY,
    SYSTEM_INFO_CTOR_PRIORITY,
    BUFFER_SET_CTOR_PRIORITY,
    EC_ENCODE_CACHE_POOL_CTOR_PRIORITY,
    LCTLI_SUBSYS_CTOR_PRIORITY,
    UTIL_THREAD_SUBSYS_CTOR_PRIORITY,
    CONFIG_TOKEN_CTOR_PRIORITY,
    CTL_SVC_CTOR_PRIORITY,
    VBLKDEV_HANDLE_CTOR_PRIORITY,
    RAFT_SYS_CTOR_PRIORITY,
    RAFT_CLIENT_CTOR_PRIORITY,
    LCTLI_SUBSYS_ENABLE_CTOR_PRIORITY,
    UNIT_TEST_CTOR_PRIORITY,
    INIT_COMPLETE_CTOR_PRIORITY,
};

#endif
