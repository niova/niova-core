/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */
#ifndef NIOVA_LOG_H
#define NIOVA_LOG_H 1

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#include <stdio.h>
#include <pthread.h>
#undef _GNU_SOURCE
#endif

#include "common.h"

#include "init.h"
#include "thread.h"
#include "util.h"
#include "registry.h"

LREG_ROOT_ENTRY_EXPORT(log_entry_map);

enum log_level
{
    LL_FATAL  = 0,
    LL_ERROR  = 1,
    LL_WARN   = 2,
    LL_NOTIFY = 3,
    LL_DEBUG  = 4,
    LL_TRACE  = 5,
    LL_MAX    = 6,
    LL_ANY    = LL_MAX,
};

enum log_level
log_level_get(void);

#define LL_STRING_LEN_MAX 8

static inline const char *
ll_to_string(enum log_level ll)
{
    switch (ll)
    {
    case LL_ANY:
        return "default";
    case LL_FATAL:
        return "fatal";
    case LL_ERROR:
        return "error";
    case LL_WARN:
        return "warn";
    case LL_NOTIFY:
        return "notify";
    case LL_DEBUG:
        return "debug";
    case LL_TRACE:
        return "trace";
    default:
        break;
    }

    return "unknown";
}

static inline enum log_level
ll_from_string(const char *log_level_string)
{
    for (enum log_level lvl = 0; lvl < LL_MAX; lvl++)
        if (!strncmp(log_level_string, ll_to_string(lvl), LL_STRING_LEN_MAX))
            return lvl;

    return LL_ANY;
}

struct log_entry_info
{
    enum log_level lei_level;
    unsigned int   lei_lineno : 18;
    size_t         lei_exec_cnt;
    const char    *lei_tag;
#if 0
    size_t         lei_exec_cnt_since_last_reset;
    time_t         lei_exec_cnt_last_reset;
#endif
    union
    {
        const char *lei_func;
        const char *lei_file;
    };
};

#define REGISTRY_ENTRY_FILE_GENERATE                                    \
    static struct log_entry_info logEntryFileInfo = {                   \
        .lei_level = LL_ANY,                                            \
        .lei_file = __FILE__,                                           \
    };                                                                  \
                                                                        \
    static struct lreg_node regFileEntry = {                            \
        .lrn_cb_arg = &logEntryFileInfo,                                \
        .lrn_user_type = LREG_USER_TYPE_LOG_file,                       \
        .lrn_statically_allocated = 1,                                  \
        .lrn_array_element = 1,                                         \
        .lrn_cb = log_lreg_cb,                                          \
        .lrn_inlined_children = 1,                                      \
        .lrn_head = CIRCLEQ_HEAD_INITIALIZER(regFileEntry.lrn_head),    \
    }

#define REGISTY_ENTRY_FUNCTION_GENERATE(tag)                            \
    static struct log_entry_info logEntryInfo = {                       \
        .lei_level = LL_ANY,                                            \
        .lei_lineno = __LINE__,                                         \
        .lei_func = __func__,                                           \
        .lei_tag = tag,                                                 \
    };                                                                  \
    static struct lreg_node logMsgLrn = {                               \
        .lrn_cb_arg = &logEntryInfo,                                    \
        .lrn_user_type = LREG_USER_TYPE_LOG_func,                       \
        .lrn_statically_allocated = 1,                                  \
        .lrn_array_element = 1,                                         \
        .lrn_inlined_member = 1,                                        \
        .lrn_cb = log_lreg_cb,                                          \
    };                                                                  \
    int _node_install_rc = 0;                                           \
    if (lreg_node_needs_installation(&logMsgLrn))                       \
    {                                                                   \
        _node_install_rc = lreg_node_install(&logMsgLrn, &regFileEntry); \
        NIOVA_ASSERT(!_node_install_rc ||                               \
                     _node_install_rc == -EALREADY ||                   \
                     _node_install_rc == -EAGAIN);                      \
    }                                                                   \
    if (lreg_node_needs_installation(&regFileEntry))                    \
    {                                                                   \
        _node_install_rc =                                              \
            lreg_node_install(&regFileEntry,                            \
                              LREG_ROOT_ENTRY_PTR(log_entry_map));      \
        NIOVA_ASSERT(!_node_install_rc ||                               \
                     _node_install_rc == -EALREADY ||                   \
                     _node_install_rc == -EAGAIN);                      \
    }                                                                   \


// Allow users of _NIOVA_BACKTRACE_H_ to generate backtraces in abort ctx
#ifdef _NIOVA_BACKTRACE_H_
#define LOG_ABORT                               \
    do { niova_backtrace_dump(); thread_abort(); } while (0)
#else
#define LOG_ABORT                               \
    do { thread_abort(); } while (0)
#endif

#define SIMPLE_LOG_MSG_EXEC(usr_level, sys_level, exec, message, ...)        \
do {                                                                         \
    if ((usr_level) <= (sys_level))                                          \
    {                                                                        \
        exec;                                                                \
        struct timespec ts;                                                  \
        niova_unstable_clock(&ts);                                           \
        if (thrCoName)                                                       \
        {                                                                    \
            fprintf(stderr, "<%ld.%09lu:%s:%s,%s.%x.%x:%s@%d> " message "\n", \
                    ts.tv_sec, ts.tv_nsec, ll_to_string(usr_level),          \
                    thread_name_get(), thrCoName, (int)threadCoID,           \
                    threadCoYieldCnt, __func__,                              \
                    __LINE__, ##__VA_ARGS__);                                \
        }                                                                    \
        else                                                                 \
        {                                                                    \
            fprintf(stderr, "<%ld.%09lu:%s:%s:%s@%d> " message "\n",         \
                    ts.tv_sec, ts.tv_nsec, ll_to_string(usr_level),          \
                    thread_name_get(), __func__, __LINE__, ##__VA_ARGS__);   \
        }                                                                    \
        if ((usr_level) == LL_FATAL)                                         \
        {                                                                    \
            LOG_ABORT;                                                       \
            __builtin_unreachable();                                         \
        }                                                                    \
    }                                                                        \
} while (0)

#define SIMPLE_LOG_MSG(level, message, ...)                     \
    SIMPLE_LOG_MSG_EXEC(level, log_level_get(), {}, message, ##__VA_ARGS__)

#define FUNC_ENTRY(level) \
    LOG_MSG(level, "enter")

#define FUNC_EXIT(level) \
    LOG_MSG(level, "exit")

#define SIMPLE_FUNC_ENTRY(level) \
    SIMPLE_LOG_MSG(level, "enter")

#define SIMPLE_FUNC_EXIT(level) \
    SIMPLE_LOG_MSG(level, "exit")

/**
 * LOG_MSG - during normal runtime, take the log level first from this
 *    registry entry's level, and next from the file's level.  If neither are
 *    set, then use the level provided by the caller.
 */
#define _LOG_MSG(user_lvl, tag, exec, message, ...)                     \
do {                                                                    \
    enum log_level sys_lvl = log_level_get();                           \
                                                                        \
    if (!init_ctx())                                                    \
    {                                                                   \
        REGISTY_ENTRY_FUNCTION_GENERATE(tag);                           \
                                                                        \
        logEntryInfo.lei_exec_cnt++;                                    \
                                                                        \
        if (logEntryInfo.lei_level != LL_ANY)                           \
            sys_lvl = logEntryInfo.lei_level;                           \
                                                                        \
        else if (logEntryFileInfo.lei_level != LL_ANY)                  \
            sys_lvl = logEntryFileInfo.lei_level;                           \
    }                                                                   \
    SIMPLE_LOG_MSG_EXEC(user_lvl, sys_lvl, exec, message, ##__VA_ARGS__); \
} while (0)

#define LOG_MSG_EXEC(user_lvl, exec, message, ...)               \
    _LOG_MSG(user_lvl, NULL, exec, message, ##__VA_ARGS__)

#define LOG_MSG_TAG_EXEC(user_lvl, tag, exec, message, ...)   \
    _LOG_MSG(user_lvl, tag, exec, message, ##__VA_ARGS__)

#define LOG_MSG(user_lvl, message, ...)         \
    _LOG_MSG(user_lvl, NULL, {}, message, ##__VA_ARGS__)

#define LOG_MSG_TAG(user_lvl, tag, message, ...)        \
    _LOG_MSG(user_lvl, tag, {}, message, ##__VA_ARGS__)

#define FATAL_MSG(message, ...) \
    SIMPLE_LOG_MSG(LL_FATAL, message, ##__VA_ARGS__)

#define FATAL_IF(cond, message, ...)       \
do {                                       \
    if ((cond))                            \
    {                                      \
        FATAL_MSG(message, ##__VA_ARGS__); \
    }                                      \
} while (0)

#define FATAL_IF_strerror(cond, message, ...)                     \
do {                                                              \
    if ((cond))                                                   \
    {                                                             \
        FATAL_MSG(message": %s", ##__VA_ARGS__, strerror(errno)); \
    }                                                             \
} while (0)

#define NIOVA_ASSERT(cond)                    \
do {                                          \
    if (!(cond))                              \
        FATAL_MSG("failed assertion: "#cond); \
} while (0)

#define NIOVA_ASSERT_strerror(cond)                                  \
do {                                                                 \
    if (!(cond))                                                     \
        FATAL_MSG("failed assertion: "#cond", %s", strerror(errno)); \
} while (0)

#define EXIT_ERROR_MSG(exit_val, message, ...)        \
do {                                                  \
    SIMPLE_LOG_MSG(LL_ERROR, message, ##__VA_ARGS__); \
    exit(exit_val);                                   \
} while (0)

#define DEBUG_BLOCK(lvl) \
    if (lvl <= log_level_get())

#define log_msg LOG_MSG

#define STDOUT_MSG(message, ...)                 \
do {                                             \
    fprintf(stdout, "<%lx:%s@%d> " message "\n", \
            thread_id_get(), __func__,           \
            __LINE__,##__VA_ARGS__);             \
} while (0)

#define STDERR_MSG(message, ...)                 \
do {                                             \
    fprintf(stderr, "<%lx:%s@%d> " message "\n", \
            thread_id_get(), __func__,           \
            __LINE__,##__VA_ARGS__);             \
} while (0)

static inline bool
log_level_is_valid(unsigned int level)
{
    return level < LL_MAX ? true : false;
}

void
log_level_set(enum log_level);

void
log_level_set_from_env(void);

int
log_lreg_cb(enum lreg_node_cb_ops, struct lreg_node *lrn, struct lreg_value *);

init_ctx_t
log_subsys_init(void) __attribute__ ((constructor (LOG_SUBSYS_CTOR_PRIORITY)));

destroy_ctx_t
log_subsys_destroy(void)
__attribute__ ((destructor (LOG_SUBSYS_CTOR_PRIORITY)));

#endif //NIOVA_LOG_H
