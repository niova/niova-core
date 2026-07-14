/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */

#ifndef _REGISTRY_H
#define _REGISTRY_H 1

#include "atomic.h"
#include "ctor.h"
#include "init.h"
#include "queue.h"
#include "util.h"
#include "util_thread.h"

/* "Install and destroy context" is designated for threads other than the
 * lreg service thread which may add new registry objects.
 */
typedef void             lreg_install_ctx_t;
typedef void             lreg_destroy_ctx_t;
typedef int              lreg_install_int_ctx_t;
typedef int              lreg_destroy_int_ctx_t;
typedef bool             lreg_install_bool_ctx_t;
typedef void             lreg_svc_ctx_t;
typedef void             * lreg_svc_thread_t;
typedef struct lreg_node * lreg_svc_lrn_ctx_t;
typedef int              lreg_svc_int_ctx_t;
typedef void             lreg_user_ctx_t;
typedef int              lreg_user_int_ctx_t;

typedef bool (*lrn_walk_cb_t)(struct lreg_node *, void *, const int);

struct lreg_value;
typedef void (*lrn_recurse_cb_t)(struct lreg_value *, const int, const int,
                                 const bool);

#define LREG_VALUE_STRING_MAX 255
#define LREG_NODE_KEYS_MAX 65536

enum lreg_value_types
{
    LREG_VAL_TYPE_NONE,
    LREG_VAL_TYPE_ARRAY,
    LREG_VAL_TYPE_VARRAY, // virtual array (not attached to an lreg_node)
    LREG_VAL_TYPE_VOBJECT, // virtual object (not attached to an lreg_node)
    LREG_VAL_TYPE_BOOL,
    LREG_VAL_TYPE_OBJECT,
    LREG_VAL_TYPE_ANON_OBJECT,
    LREG_VAL_TYPE_SIGNED_VAL,
    LREG_VAL_TYPE_SIGNED32_VAL,
    LREG_VAL_TYPE_STRING,
    LREG_VAL_TYPE_UNSIGNED_VAL,
    LREG_VAL_TYPE_FLOAT_VAL,
    LREG_VAL_TYPE_ANY,
    LREG_VAL_TYPE_HISTOGRAM_OBJECT = LREG_VAL_TYPE_OBJECT, // for now..
} PACKED;

enum lreg_user_types
{
    LREG_USER_TYPE_NONE = 0,
    LREG_USER_TYPE_CTL_SVC_NODE,
    LREG_USER_TYPE_CTL_INTERFACE,
    LREG_USER_TYPE_CTL_INTERFACE_ROP,
    LREG_USER_TYPE_FAULT_INJECT,
    LREG_USER_TYPE_HISTOGRAM0,
    LREG_USER_TYPE_HISTOGRAM1,
    LREG_USER_TYPE_HISTOGRAM2,
    LREG_USER_TYPE_HISTOGRAM3,
    LREG_USER_TYPE_HISTOGRAM4,
    LREG_USER_TYPE_HISTOGRAM5,
    LREG_USER_TYPE_HISTOGRAM6,
    LREG_USER_TYPE_HISTOGRAM8,
    LREG_USER_TYPE_HISTOGRAM9,
    LREG_USER_TYPE_HISTOGRAM10,
    LREG_USER_TYPE_HISTOGRAM11,
    LREG_USER_TYPE_HISTOGRAM12,
    LREG_USER_TYPE_HISTOGRAM13,
    LREG_USER_TYPE_HISTOGRAM14,
    LREG_USER_TYPE_HISTOGRAM15,
    LREG_USER_TYPE_HISTOGRAM16,
    LREG_USER_TYPE_HISTOGRAM17,
    LREG_USER_TYPE_HISTOGRAM18,
    LREG_USER_TYPE_HISTOGRAM19,
    LREG_USER_TYPE_HISTOGRAM20,
    LREG_USER_TYPE_HISTOGRAM21,
    LREG_USER_TYPE_HISTOGRAM22,
    LREG_USER_TYPE_HISTOGRAM23,
    LREG_USER_TYPE_HISTOGRAM24,
    LREG_USER_TYPE_HISTOGRAM25,
    LREG_USER_TYPE_HISTOGRAM26,
    LREG_USER_TYPE_HISTOGRAM28,
    LREG_USER_TYPE_HISTOGRAM29,
    LREG_USER_TYPE_HISTOGRAM__MAX,
    LREG_USER_TYPE_LOG_file,
    LREG_USER_TYPE_LOG_func,
    LREG_USER_TYPE_LOG_subsys,
    LREG_USER_TYPE_NIOSD_IO,
    LREG_USER_TYPE_NIOSD_IO_CTX,
    LREG_USER_TYPE_NIOSD_IO_STATS,
    LREG_USER_TYPE_RAFT,
    LREG_USER_TYPE_RAFT_NET,
    LREG_USER_TYPE_RAFT_RECOVERY_NET,
    LREG_USER_TYPE_RAFT_CLIENT,
    LREG_USER_TYPE_RAFT_CLIENT_APP,
    LREG_USER_TYPE_RAFT_CLIENT_APP_DATA,
    LREG_USER_TYPE_RAFT_CLIENT_ROP_RD,
    LREG_USER_TYPE_RAFT_CLIENT_ROP_WR,
    LREG_USER_TYPE_RAFT_CLIENT_PENDING_OP,
    LREG_USER_TYPE_RAFT_PEER_STATS,
    LREG_USER_TYPE_ROOT,
    LREG_USER_TYPE_SYS_INFO,
    LREG_USER_TYPE_UNIT_TEST0,
    LREG_USER_TYPE_UNIT_TEST1,
    LREG_USER_TYPE_UNIT_TEST2,
    LREG_USER_TYPE_UNIT_TEST3,
    LREG_USER_TYPE_UNIT_TEST4,
    LREG_USER_TYPE_NISD_IOMGR,
    LREG_USER_TYPE_NISD,
    LREG_USER_TYPE_NISD_CHUNK,
    LREG_USER_TYPE_MB_MERGE,
    LREG_USER_TYPE_IOPM_CONN,
    LREG_USER_TYPE_NIOVA_TASK,
    LREG_USER_TYPE_NIOVA_CHUNK_SNAP,
    LREG_USER_TYPE_NIOVA_CHUNK_DEFRAG,
    LREG_USER_TYPE_NIOVA_CHUNK_SCRUB,
    LREG_USER_TYPE_MB_ROLLBACK,
    LREG_USER_TYPE_BUFFER_SET,
    LREG_USER_TYPE_SNAPSHOT,
    LREG_USER_TYPE_ANY,
    LREG_USER_TYPE_HISTOGRAM = LREG_USER_TYPE_HISTOGRAM0,
    LREG_USER_TYPE_HISTOGRAM__MIN = LREG_USER_TYPE_HISTOGRAM0,
    LREG_USER_TYPE__LAST,
} PACKED;

static inline enum lreg_user_types
lreg_user_type_histogram_convert(const int hist_idx)
{
    const int max_hist_idx =
        LREG_USER_TYPE_HISTOGRAM__MAX - LREG_USER_TYPE_HISTOGRAM__MIN;

    enum lreg_user_types ret_idx =
        hist_idx <= max_hist_idx ? hist_idx : LREG_USER_TYPE_HISTOGRAM__MAX;

    return ret_idx;
}

enum lreg_node_cb_ops
{
    LREG_NODE_CB_OP_GET_NODE_INFO,
    LREG_NODE_CB_OP_WRITE_VAL,
    LREG_NODE_CB_OP_READ_VAL,
    LREG_NODE_CB_OP_INSTALL_QUEUED_NODE, // node queued for async install
    LREG_NODE_CB_OP_INSTALL_NODE,        // node is being installed
    LREG_NODE_CB_OP_DESTROY_NODE,        // node is removed from the subsys
} PACKED;

#define LREG_NODE_CB_OP_GET_NAME LREG_NODE_CB_OP_GET_NODE_INFO

union lreg_value_data_numeric
{
    uint64_t lrvdn_unsigned_val;
    int64_t  lrvdn_signed_val;
    float    lrvdn_float_val;
    bool     lrvdn_bool_val;
};

struct lreg_value_data
{
    union
    {
        uint64_t lrv_unsigned_val;
        int64_t  lrv_signed_val;
        float    lrv_float;
        bool     lrv_bool;
        char     lrv_string[LREG_VALUE_STRING_MAX + 1];
    };
};

enum lreg_init_options
{
    LREG_INIT_OPT_NONE                = 0,
    LREG_INIT_OPT_STATIC              = 1 << 0,
    LREG_INIT_OPT_IGNORE_NUM_VAL_ZERO = 1 << 1,
    LREG_INIT_OPT_REVERSE_VARRAY      = 1 << 2,
    LREG_INIT_OPT_INLINED_MEMBER      = 1 << 3,
    LREG_INIT_OPT_INLINED_CHILDREN    = 1 << 4,
};

struct lreg_node;
struct lreg_value;
typedef int (*lrn_cb_t)(enum lreg_node_cb_ops, struct lreg_node *,
                        struct lreg_value *);

struct lreg_value_vnode_data
{
    unsigned int lvvd_num_keys_out;
    uint8_t      lvvd_vobject : 1; // if '0', it's a varray
    lrn_cb_t     lvvd_cb;
};

/**
 * -- struct lreg_value --
 * Complex value structure used for obtained multi-faceted object values from
 * local registry nodes.
 * - GET operation:
 * @lrv_value_idx_in:  logical value index for a value "get" operation.
 * @lrv_op_in:  the op from which the results were produced.
 * @lrv_num_keys_out:  number of values found at this object.
 * @lrv_node_type_out:  value type corresponding to the requested index.
 * @lrv_value_out:  storage for output value.
 * - PUT operation:
 * @lrv_key_string:  key string for the provided value.
 * @lrv_value_in:  input value storage.
 */
struct lreg_value
{
    char                  lrv_key_string[LREG_VALUE_STRING_MAX + 1];
    unsigned int          lrv_value_idx_in;
    enum lreg_node_cb_ops lrv_op_in;
    union
    {
        struct
        {
            unsigned int          lrv_num_keys_out;
            enum lreg_value_types lrv_value_type_out;
            enum lreg_user_types  lrv_user_type_out;
            union
            {
                struct lreg_value_data       lrv_value_out;
                struct lreg_value_vnode_data lrv_varray_out;
            };
        } get;

        struct
        {
            enum lreg_value_types  lrv_value_type_in;
            struct lreg_value_data lrv_value_in;
        } put;
    };
};

#define lrv_node_type_out lrv_value_type_out //xxx to be removed

#define LREG_VALUE_TO_OUT_STR(lrv) \
    (lrv)->get.lrv_value_out.lrv_string

#define LREG_VALUE_TO_KEY_STR(lrv) \
    (lrv)->lrv_key_string

#define LREG_VALUE_TO_BOOL(lrv) \
    (lrv)->get.lrv_value_out.lrv_bool

#define LREG_VALUE_TO_IN_STR(lrv) \
    (lrv)->put.lrv_value_in.lrv_string

#define LREG_VALUE_TO_OUT_SIGNED_INT(lrv) \
    (lrv)->get.lrv_value_out.lrv_signed_val

#define LREG_VALUE_TO_OUT_UNSIGNED_INT(lrv) \
    (lrv)->get.lrv_value_out.lrv_unsigned_val

#define LREG_VALUE_TO_OUT_FLOAT(lrv) \
    (lrv)->get.lrv_value_out.lrv_float

#define LREG_VALUE_TO_REQ_TYPE(lrv) \
    (lrv)->get.lrv_value_type_out

#define LREG_VALUE_TO_REQ_TYPE_IN(lrv) \
    (lrv)->put.lrv_value_type_in

#define LREG_VALUE_TO_USER_TYPE(lrv) \
    (lrv)->get.lrv_user_type_out

struct lreg_vnode_data
{
    enum lreg_user_types lvd_user_type;
    uint8_t              lvd_varray  : 1;
    uint8_t              lvd_vobject : 1;
    union
    {
        uint32_t lvd_index;
        uint32_t lvd_num_entries;
    };
};

/* LREG_NODE_NOT_INSTALLED and LREG_NODE_REMOVED purposely exist so that
 * lreg_node_wait_for_install_state_change() will not remain waiting while
 * another thread transitions the node through all it states.  Therefore,
 * the final state for a node is 'LREG_NODE_REMOVED'. Users seeking to reinstall
 * nodes must be mindful that nothing uses the node so that the node can be
 * safely transitioned from LREG_NODE_REMOVED -> LREG_NODE_NOT_INSTALLED.
 */
enum lreg_node_states
{
    LREG_NODE_NOT_INSTALLED = 0,
    LREG_NODE_INSTALLING    = (1 << 0),
    LREG_NODE_INSTALLED     = (1 << 1),
    LREG_NODE_REMOVING      = (1 << 2),
    LREG_NODE_REMOVED       = (1 << 3)
} PACKED;

struct lreg_node;
CIRCLEQ_HEAD(lreg_node_list, lreg_node);

STAILQ_HEAD(lreg_destroy_queue, lreg_node);

/**
 * -- struct lreg_node --
 */
struct lreg_node
{
    enum lreg_user_types  lrn_user_type;
    enum lreg_node_states lrn_install_state;
    uint8_t              lrn_tmp_node                     : 1,
                         lrn_statically_allocated         : 1,
                         lrn_root_node                    : 1,
                         lrn_monitor                      : 1,
                         lrn_may_destroy                  : 1,
                         lrn_array_element                : 1,
                         lrn_ignore_items_with_value_zero : 1,
                         lrn_reverse_varray               : 1,
                         lrn_vnode_child                  : 1,
                         lrn_inlined_member               : 1,
                         lrn_inlined_children             : 1,
                         lrn_async_install                : 1,
                         lrn_async_remove                 : 1;
    void                    *lrn_cb_arg;
    //xxx lrn_cb can be moved into a static array indexed by lrn_user_type
    lrn_cb_t                 lrn_cb;
    CIRCLEQ_ENTRY(lreg_node) lrn_lentry;
    union
    {
        struct lreg_node_list   lrn_head; //arrays and objects
        struct lreg_vnode_data  lrn_lvd;
        struct lreg_node       *lrn_parent_for_remove_only;
    };
    union
    {
        struct lreg_node       *lrn_parent_for_install_only;
        STAILQ_ENTRY(lreg_node) lrn_removal_lentry;
    };
};

static inline void
lreg_value_vnode_data_to_lreg_node(const struct lreg_value *lrv,
                                   struct lreg_node *lrn)

{
    if (!lrv || !lrn)
        return;

    lrn->lrn_vnode_child = 1;
    lrn->lrn_cb = lrv->get.lrv_varray_out.lvvd_cb;

    lrn->lrn_lvd.lvd_user_type = LREG_VALUE_TO_USER_TYPE(lrv);
    lrn->lrn_lvd.lvd_num_entries = lrv->get.lrv_varray_out.lvvd_num_keys_out;
    lrn->lrn_lvd.lvd_varray = !lrv->get.lrv_varray_out.lvvd_vobject;
    lrn->lrn_lvd.lvd_vobject = lrv->get.lrv_varray_out.lvvd_vobject;
}

static inline char
lreg_val_to_type(enum lreg_value_types type)
{
    switch (type)
    {
    case LREG_VAL_TYPE_NONE:
        return 'n';
    case LREG_VAL_TYPE_ARRAY:
        return 'A';
    case LREG_VAL_TYPE_BOOL:
        return 'B';
    case LREG_VAL_TYPE_OBJECT:
        return 'O';
    case LREG_VAL_TYPE_ANON_OBJECT:
        return 'o';
    case LREG_VAL_TYPE_SIGNED_VAL:
        return 'S';
    case LREG_VAL_TYPE_UNSIGNED_VAL:
        return 'U';
    default:
        break;
    }
    return 'a';
}

static inline char
lreg_node_to_user_type(const struct lreg_node *lrn)
{
    switch (lrn->lrn_user_type)
    {
    case LREG_USER_TYPE_NONE:
        return 'n';
    case LREG_USER_TYPE_FAULT_INJECT:
        return 'f';
    case LREG_USER_TYPE_LOG_file:
        return 'L';
    case LREG_USER_TYPE_LOG_func:
        return 'l';
    case LREG_USER_TYPE_ROOT:
        return 'R';
    default:
        break;
    }
    return 'a';
}

static inline char
lreg_node_to_install_state(const struct lreg_node *lrn)
{
    switch (lrn->lrn_install_state)
    {
    case LREG_NODE_NOT_INSTALLED:
        return '-';
    case LREG_NODE_INSTALLING:
        return 'i';
    case LREG_NODE_INSTALLED:
        return 'I';
    case LREG_NODE_REMOVING:
        return 'r';
    case LREG_NODE_REMOVED:
        return 'R';
    default:
        break;
    }
    return '?';
}

#define DBG_LREG_NODE(log_level, lrn, fmt, ...)                    \
do {                                                               \
    struct lreg_value lrv = {0};                                   \
    SIMPLE_LOG_MSG(log_level,                                      \
                   "lrn@%p %s %c%c %c%c%c%c%c%c%c%c%c%c%c arg=%p "fmt,   \
                   (lrn),                                          \
                   (const char *)({                                \
                           (lrn)->lrn_cb(LREG_NODE_CB_OP_GET_NAME, \
                                         (lrn), &lrv);             \
                           LREG_VALUE_TO_KEY_STR(&lrv);            \
                       }),                                         \
                   lreg_node_to_user_type(lrn),                    \
                   lreg_node_to_install_state(lrn),                \
                   (lrn)->lrn_statically_allocated  ? 's' : '-',   \
                   (lrn)->lrn_tmp_node              ? 't' : '-',   \
                   (lrn)->lrn_root_node             ? 'r' : '-',   \
                   (lrn)->lrn_may_destroy           ? 'd' : '-',   \
                   (lrn)->lrn_monitor               ? 'm' : '-',   \
                   (lrn)->lrn_array_element         ? 'a' : '-',   \
                   (lrn)->lrn_vnode_child           ? 'v' : '-',        \
                   (lrn)->lrn_inlined_member        ? 'i' : '-',        \
                   (lrn)->lrn_inlined_children      ? 'C' : '-',    \
                   (lrn)->lrn_async_install         ? 'A' : '-',        \
                   (lrn)->lrn_async_remove          ? 'R' : '-',        \
                   (lrn)->lrn_cb_arg, ##__VA_ARGS__);              \
} while (0)

static inline bool
lreg_statically_allocated_node_check(const struct lreg_node *lrn)
{
    return (lrn->lrn_statically_allocated && !lrn->lrn_may_destroy) ?
        true : false;
}


static inline bool
lreg_node_needs_installation(const struct lreg_node *lrn)
{
    return lrn->lrn_install_state == LREG_NODE_NOT_INSTALLED ? true : false;
}

static inline bool
lreg_node_is_installed(const struct lreg_node *lrn)
{
    return lrn->lrn_install_state == LREG_NODE_INSTALLED ? true : false;
}

static inline bool
lreg_node_is_removed(const struct lreg_node *lrn)
{
    return (lrn->lrn_install_state == LREG_NODE_REMOVED);
}

static inline bool
lreg_node_install_prep_ok(struct lreg_node *lrn)
{
    return niova_atomic_cas(&lrn->lrn_install_state, LREG_NODE_NOT_INSTALLED,
                            LREG_NODE_INSTALLING) ? true : false;
}

static inline bool
lreg_node_install_complete(struct lreg_node *lrn)
{
    return niova_atomic_cas(&lrn->lrn_install_state, LREG_NODE_INSTALLING,
                            LREG_NODE_INSTALLED) ? true : false;
}

static inline bool
lreg_node_set_removing(struct lreg_node *lrn)
{
    return niova_atomic_cas(&lrn->lrn_install_state, LREG_NODE_INSTALLED,
                            LREG_NODE_REMOVING) ? true : false;
}

static inline bool
lreg_node_set_uninstalled(struct lreg_node *lrn)
{
    return niova_atomic_cas(&lrn->lrn_install_state, LREG_NODE_REMOVING,
                            LREG_NODE_REMOVED) ? true : false;
}

static inline bool
lreg_node_has_children(const struct lreg_node *lrn)
{
    return CIRCLEQ_EMPTY(&lrn->lrn_head) ? false : true;
}

static inline bool
lreg_node_children_are_inlined(const struct lreg_node *lrn)
{
    return lrn->lrn_inlined_children ? true : false;
}

static inline int
lreg_node_exec_lrn_cb(enum lreg_node_cb_ops op, struct lreg_node *lrn,
                      struct lreg_value *lrv)
{
    if (!lrn)
        return -EINVAL;

    if (lrv)
        lrv->lrv_op_in = op;

    return lrn->lrn_cb(op, lrn, lrv);
}

struct lreg_node *
lreg_root_node_get(void);

lreg_install_int_ctx_t
lreg_node_install(struct lreg_node *, struct lreg_node *);

lreg_destroy_int_ctx_t
lreg_node_remove(struct lreg_node *, struct lreg_node *);

void
lreg_node_init(struct lreg_node *, enum lreg_user_types, lrn_cb_t, void *,
               enum lreg_init_options);

lreg_install_ctx_t
lreg_node_object_init(struct lreg_node *, enum lreg_user_types, bool);

#define LREG_ROOT_ENTRY_GENERATE(name, user_type)                       \
    static lreg_install_int_ctx_t                                       \
    lreg_root_cb##name(enum lreg_node_cb_ops op, struct lreg_node *lrn, \
                       struct lreg_value *lreg_val)                     \
    {                                                                   \
	(void)lrn;							\
        switch (op)                                                     \
        {                                                               \
        case LREG_NODE_CB_OP_GET_NODE_INFO:                             \
            if (!lreg_val)                                              \
                return -EINVAL;                                         \
                                                                        \
            lreg_val->get.lrv_num_keys_out = 1;                         \
            snprintf(lreg_val->lrv_key_string,                          \
                     LREG_VALUE_STRING_MAX, #name);                     \
            break;                                                      \
        case LREG_NODE_CB_OP_READ_VAL:                                  \
            if (!lreg_val)                                              \
                return -EINVAL;                                         \
            if (lreg_val->lrv_value_idx_in != 0)                        \
                return -ERANGE;                                         \
            lreg_val->get.lrv_value_type_out = LREG_VAL_TYPE_ARRAY;     \
            lreg_val->get.lrv_user_type_out = user_type;                \
            snprintf(lreg_val->lrv_key_string,                          \
                     LREG_VALUE_STRING_MAX, #name);                     \
            break;                                                      \
        case LREG_NODE_CB_OP_WRITE_VAL:                                 \
            return -EOPNOTSUPP;                                         \
        case LREG_NODE_CB_OP_INSTALL_NODE: /* fall through */           \
        case LREG_NODE_CB_OP_DESTROY_NODE: /* fall through */           \
        case LREG_NODE_CB_OP_INSTALL_QUEUED_NODE:                       \
            break;                                                      \
        default:                                                        \
            return -ENOENT;                                             \
        }                                                               \
        return 0;                                                       \
    }                                                                   \
                                                                        \
    struct lreg_node rootEntry__##name = {                              \
        .lrn_cb_arg = (void *)1,                                        \
        .lrn_user_type = user_type,                                     \
        .lrn_statically_allocated = 1,                                  \
        .lrn_cb = lreg_root_cb##name                                    \
    }                                                                   \

#define LREG_ROOT_ENTRY_GENERATE_TYPE(name, user_type, num_keys,    \
                                      read_write_op_cb, arg, type, opts)  \
                                                                     \
    _Pragma("GCC diagnostic push")                                   \
    _Pragma("GCC diagnostic ignored \"-Wunknown-pragmas\"")          \
    _Pragma("clang diagnostic push")                                 \
    _Pragma("clang diagnostic ignored \"-Wunused-function\"")        \
    static inline void                                               \
    lreg_compile_time_assert##name(enum lreg_value_types type)       \
    {                                                               \
        COMPILE_TIME_ASSERT(type == LREG_VAL_TYPE_ARRAY ||          \
                            type == LREG_VAL_TYPE_OBJECT);          \
    }                                                               \
    _Pragma("clang diagnostic pop")                                 \
    _Pragma("GCC diagnostic pop")                                  \
                                                                    \
    static lreg_install_int_ctx_t                                   \
    lreg_root_cb_child##name(enum lreg_node_cb_ops op,              \
                             struct lreg_node *lrn,                 \
                             struct lreg_value *lreg_val)           \
    {                                                               \
        int rc = 0;                                                 \
	(void)lrn;						    \
        switch (op)                                                 \
        {                                                           \
        case LREG_NODE_CB_OP_GET_NODE_INFO:                         \
            if (!(lreg_val))                                        \
                return -EINVAL;                                     \
            lreg_val->get.lrv_num_keys_out = num_keys;              \
            snprintf(lreg_val->lrv_key_string,                      \
                     LREG_VALUE_STRING_MAX, #name);                 \
            break;                                                  \
        case LREG_NODE_CB_OP_READ_VAL:     /* fall through */       \
        case LREG_NODE_CB_OP_WRITE_VAL:                             \
            if (!(lreg_val))                                        \
                return -EINVAL;                                     \
            if (lreg_val->lrv_value_idx_in >= num_keys)             \
                return -ERANGE;                                     \
            rc = read_write_op_cb(op, lreg_val, (lrn)->lrn_cb_arg); \
            break;                                                  \
        case LREG_NODE_CB_OP_INSTALL_NODE: /* fall through */       \
        case LREG_NODE_CB_OP_DESTROY_NODE: /* fall through */       \
        case LREG_NODE_CB_OP_INSTALL_QUEUED_NODE:                   \
            break;                                                  \
        default:                                                    \
            return -EOPNOTSUPP;                                     \
        }                                                           \
        return rc;                                                  \
    }                                                               \
    static lreg_install_int_ctx_t                                   \
    lreg_root_cb_parent##name(enum lreg_node_cb_ops op,             \
                              struct lreg_node *lrn,                \
                              struct lreg_value *lreg_val)          \
    {                                                               \
	(void)lrn;						    \
	switch (op)						    \
        {                                                           \
        case LREG_NODE_CB_OP_GET_NODE_INFO:                         \
            if (!lreg_val)                                          \
                return -EINVAL;                                     \
                                                                    \
            lreg_val->get.lrv_num_keys_out = 1;                     \
            snprintf(lreg_val->lrv_key_string,                      \
                     LREG_VALUE_STRING_MAX, #name);                 \
            break;                                                  \
        case LREG_NODE_CB_OP_READ_VAL:                              \
            if (!lreg_val)                                          \
                return -EINVAL;                                     \
            if (lreg_val->lrv_value_idx_in != 0)                    \
                return -ERANGE;                                     \
            lreg_val->get.lrv_value_type_out = type;                \
            lreg_val->get.lrv_user_type_out = user_type;            \
            snprintf(lreg_val->lrv_key_string,                      \
                     LREG_VALUE_STRING_MAX, #name);                 \
            break;                                                  \
        case LREG_NODE_CB_OP_WRITE_VAL:                             \
            return -EOPNOTSUPP;                                     \
        case LREG_NODE_CB_OP_INSTALL_NODE: /* fall through */       \
        case LREG_NODE_CB_OP_DESTROY_NODE: /* fall through */       \
        case LREG_NODE_CB_OP_INSTALL_QUEUED_NODE:                   \
            break;                                                  \
        default:                                                    \
            return -ENOENT;                                         \
        }                                                           \
        return 0;                                                   \
    }                                                               \
                                                                    \
    struct lreg_node rootEntry__##name = {                          \
        .lrn_cb_arg = (void *)1,                                    \
        .lrn_user_type = user_type,                                 \
        .lrn_statically_allocated = 1,                              \
        .lrn_cb = lreg_root_cb_parent##name,                        \
    };                                                              \
    struct lreg_node childEntry__##name = {                         \
        .lrn_user_type = user_type,                                 \
        .lrn_statically_allocated = 1,                                  \
        .lrn_reverse_varray = !!(opts & LREG_INIT_OPT_REVERSE_VARRAY),  \
        .lrn_ignore_items_with_value_zero =                             \
            !!(opts & LREG_INIT_OPT_IGNORE_NUM_VAL_ZERO),                   \
        .lrn_cb = lreg_root_cb_child##name,                         \
        .lrn_cb_arg = arg,                                          \
    };                                                              \

#define LREG_ROOT_ENTRY_GENERATE_OBJECT(name, user_type, num_keys, \
                                        read_write_op_cb, arg, opts)    \
    LREG_ROOT_ENTRY_GENERATE_TYPE(name, user_type, num_keys,       \
                                  read_write_op_cb, arg,           \
                                  LREG_VAL_TYPE_OBJECT, opts)

#define LREG_ROOT_ENTRY_EXPORT(name) \
    extern struct lreg_node rootEntry__##name

#define LREG_ROOT_ENTRY_PTR(name) \
    &rootEntry__##name

#define LREG_ROOT_ENTRY_INSTALL(name)                                  \
    NIOVA_ASSERT(!lreg_node_install(LREG_ROOT_ENTRY_PTR(name), \
                                    lreg_root_node_get()))

#define LREG_ROOT_ENTRY_REMOVE(name)                                  \
    NIOVA_ASSERT(!lreg_node_remove(LREG_ROOT_ENTRY_PTR(name), \
                                   lreg_root_node_get()))

// Don't crash if the node is already installed.
#define LREG_ROOT_ENTRY_INSTALL_ALREADY_OK(name)                   \
do {                                                                  \
    int rc = lreg_node_install(LREG_ROOT_ENTRY_PTR(name),  \
                                       lreg_root_node_get());      \
    NIOVA_ASSERT(!rc || rc == -EALREADY);                          \
} while (0)

#define LREG_ROOT_OBJECT_ENTRY_INSTALL(name)                    \
do {                                                               \
    LREG_ROOT_ENTRY_INSTALL(name);                              \
                                                                \
    NIOVA_ASSERT(                                                       \
        !lreg_node_install(&childEntry__##name, LREG_ROOT_ENTRY_PTR(name))); \
} while (0)

#define LREG_ROOT_OBJECT_ENTRY_INSTALL_RESCAN_LCTLI(name)                    \
do {                                                               \
    LREG_ROOT_ENTRY_INSTALL(name);                              \
                                                                \
    NIOVA_ASSERT(                                                       \
        !lreg_node_install(&childEntry__##name, LREG_ROOT_ENTRY_PTR(name))); \
    lreg_node_wait_for_completion(&childEntry__##name, true);           \
    NIOVA_ASSERT(                                                       \
        !lctli_init_subdir_rescan(childEntry__##name.lrn_user_type));   \
} while (0)

#define LREG_ROOT_OBJECT_ENTRY_REMOVE(name)                    \
do {                                                               \
    LREG_ROOT_ENTRY_INSTALL(name);                              \
                                                                \
    NIOVA_ASSERT(                                                       \
        !lreg_node_remove(&childEntry__##name, LREG_ROOT_ENTRY_PTR(name))); \
} while (0)

static inline void
lreg_node_set_reverse_varray(struct lreg_node *lrn)
{
    if (lrn)
        lrn->lrn_reverse_varray = 1;
}

void
lreg_node_walk(const struct lreg_node *parent, lrn_walk_cb_t lrn_wcb,
               void *cb_arg, const int depth,
               const enum lreg_user_types user_type);

static inline void
lreg_value_fill_key_and_type(struct lreg_value *lv, const char *key,
                             const enum lreg_value_types type)
{
    if (lv)
    {
        if (key)
            strncpy(lv->lrv_key_string, key, LREG_VALUE_STRING_MAX);

        LREG_VALUE_TO_REQ_TYPE(lv) = type;
    }

}

static inline void
lreg_value_fill_string(struct lreg_value *lv, const char *key,
                       const char *value)
{
    if (lv)
    {
        lreg_value_fill_key_and_type(lv, key, LREG_VAL_TYPE_STRING);

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
        if (value)
            strncpy(LREG_VALUE_TO_OUT_STR(lv), value, LREG_VALUE_STRING_MAX);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    }
}

static inline void
lreg_value_fill_string_uuid(struct lreg_value *lv, const char *key,
                            const uuid_t uuid)
{
    if (lv)
    {
        char uuid_str[UUID_STR_LEN] = {0};
        uuid_unparse(uuid, uuid_str);

        lreg_value_fill_string(lv, key, uuid_str);
    }
}

static inline void
lreg_value_fill_string_time(struct lreg_value *lv, const char *key,
                            const time_t time)
{
    if (lv)
    {
        char time_buf[MK_TIME_STR_LEN];
        int rc = niova_mk_time_string(time, time_buf, MK_TIME_STR_LEN);

        if (!rc)
            lreg_value_fill_string(lv, key, time_buf);
    }
}

static inline bool
lreg_value_is_numeric(const struct lreg_value *lv)
{
    if (lv &&
        (LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_SIGNED_VAL ||
         LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_UNSIGNED_VAL ||
         LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_FLOAT_VAL))
        return true;

    return false;
}

static inline bool
lreg_in_value_is_numeric(const struct lreg_value *lv)
{
    if (lv &&
        (LREG_VALUE_TO_REQ_TYPE_IN(lv) == LREG_VAL_TYPE_SIGNED_VAL ||
         LREG_VALUE_TO_REQ_TYPE_IN(lv) == LREG_VAL_TYPE_UNSIGNED_VAL ||
         LREG_VALUE_TO_REQ_TYPE_IN(lv) == LREG_VAL_TYPE_FLOAT_VAL))
        return true;

    return false;
}

static inline bool
lreg_value_is_array_or_object(const struct lreg_value *lv)
{
    if (lv &&
        (LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_OBJECT ||
         LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_VOBJECT ||
         LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_ANON_OBJECT ||
         LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_VARRAY ||
         LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_ARRAY))
        return true;

    return false;
}

static inline void
lreg_value_fill_numeric(struct lreg_value *lv, const char *key,
                        const union lreg_value_data_numeric lvdu,
                        const enum lreg_value_types type)
{
    if (lv && (type == LREG_VAL_TYPE_SIGNED_VAL ||
               type == LREG_VAL_TYPE_SIGNED32_VAL ||
               type == LREG_VAL_TYPE_UNSIGNED_VAL ||
               type == LREG_VAL_TYPE_FLOAT_VAL ||
               type == LREG_VAL_TYPE_BOOL))
    {
        lreg_value_fill_key_and_type(lv, key, type);
        switch (type)
        {
        case LREG_VAL_TYPE_SIGNED_VAL: // fall through
        case LREG_VAL_TYPE_SIGNED32_VAL:
            LREG_VALUE_TO_OUT_SIGNED_INT(lv) = lvdu.lrvdn_signed_val;
            break;
        case LREG_VAL_TYPE_UNSIGNED_VAL:
            LREG_VALUE_TO_OUT_UNSIGNED_INT(lv) = lvdu.lrvdn_unsigned_val;
            break;
        case LREG_VAL_TYPE_FLOAT_VAL:
            LREG_VALUE_TO_OUT_FLOAT(lv) = lvdu.lrvdn_float_val;
            break;
        case LREG_VAL_TYPE_BOOL:
            LREG_VALUE_TO_BOOL(lv) = lvdu.lrvdn_bool_val;
            break;
        default:
            break;
        }
    }
}

static inline void
lreg_value_fill_bool(struct lreg_value *lv, const char *key,
                     bool value)
{
    union lreg_value_data_numeric lvdu = {.lrvdn_bool_val = value};

    lreg_value_fill_numeric(lv, key, lvdu, LREG_VAL_TYPE_BOOL);
}

static inline void
lreg_value_fill_signed32(struct lreg_value *lv, const char *key,
                         int32_t value)
{
    union lreg_value_data_numeric lvdu = {.lrvdn_signed_val = value};

    lreg_value_fill_numeric(lv, key, lvdu, LREG_VAL_TYPE_SIGNED32_VAL);
}

static inline void
lreg_value_fill_signed(struct lreg_value *lv, const char *key,
                       int64_t value)
{
    union lreg_value_data_numeric lvdu = {.lrvdn_signed_val = value};

    lreg_value_fill_numeric(lv, key, lvdu, LREG_VAL_TYPE_SIGNED_VAL);
}

static inline void
lreg_value_fill_unsigned(struct lreg_value *lv, const char *key,
                         uint64_t value)
{
    union lreg_value_data_numeric lvdu = {.lrvdn_unsigned_val = value};

    lreg_value_fill_numeric(lv, key, lvdu, LREG_VAL_TYPE_UNSIGNED_VAL);
}

static inline void
lreg_value_fill_float(struct lreg_value *lv, const char *key, float value)
{
    union lreg_value_data_numeric lvdu = {.lrvdn_float_val = value};

    lreg_value_fill_numeric(lv, key, lvdu, LREG_VAL_TYPE_FLOAT_VAL);
}

static inline void
lreg_value_fill_array(struct lreg_value *lv, const char *key,
                      enum lreg_user_types user_type)

{
    lreg_value_fill_key_and_type(lv, key, LREG_VAL_TYPE_ARRAY);

    lv->get.lrv_user_type_out = user_type;
}

static inline void
lreg_value_fill_varray(struct lreg_value *lv, const char *key,
                       enum lreg_user_types user_type,
                       unsigned int num_entries, lrn_cb_t cb)
{
    lreg_value_fill_key_and_type(lv, key, LREG_VAL_TYPE_VARRAY);

    lv->get.lrv_user_type_out = user_type;
    lv->get.lrv_varray_out.lvvd_num_keys_out = num_entries;
    lv->get.lrv_varray_out.lvvd_cb = cb;
}

static inline void
lreg_value_fill_vobject(struct lreg_value *lv, const char *key,
                        enum lreg_user_types user_type, lrn_cb_t cb)
{
    lreg_value_fill_key_and_type(lv, key, LREG_VAL_TYPE_VOBJECT);

    lv->get.lrv_user_type_out = user_type;
    lv->get.lrv_varray_out.lvvd_num_keys_out = 1;
    lv->get.lrv_varray_out.lvvd_cb = cb;
    lv->get.lrv_varray_out.lvvd_vobject = 1;
}

static inline void
lreg_value_fill_object(struct lreg_value *lv, const char *key,
                       enum lreg_user_types user_type)

{
    lreg_value_fill_key_and_type(lv, key, LREG_VAL_TYPE_OBJECT);

    lv->get.lrv_user_type_out = user_type;
}

/**
 * lreg_value_fill_histogram - helper function for initializing an lreg value
 *    which points to a binary histogram object (binary_hist.h).
 * @lv:  lreg value pointer
 * @key:  name to be given to the key
 * @hist_idx:  numeric ID given to this histogram.  Each histogram held in an
 *    lreg_node must have a unique ID.  This value will be translated into one
 *    of the enum lreg_user_types members assigned for histograms.
 */
static inline void
lreg_value_fill_histogram(struct lreg_value *lv, const char *key,
                          const int hist_idx)

{
    lreg_value_fill_key_and_type(lv, key, LREG_VAL_TYPE_OBJECT);

    lv->get.lrv_user_type_out = lreg_user_type_histogram_convert(hist_idx);
}

/**
 * lreg_node_key_lookup - lreg tooling function which helps users perform
 *    lookups of a key name on a given lreg_node.  The function does a sanity
 *    check on the number of 'keys' the node claims to hold and will bypass
 *    the loop if that value exceeds the max.  The loop is terminated when
 *    the first string match is found and the contents of that k/v are returned
 *    to the caller through the provided lreg_value.
 */
static inline int
lreg_node_key_lookup(struct lreg_node *lrn, struct lreg_value *lv,
                     const char *key_name, size_t key_name_len)
{
    if (!lrn || !lv)
        return -EINVAL;

    int rc = lreg_node_exec_lrn_cb(LREG_NODE_CB_OP_GET_NODE_INFO, lrn, lv);
    if (rc)
        return rc;

    else if (lv->get.lrv_num_keys_out > LREG_NODE_KEYS_MAX)
        return -E2BIG;

    for (unsigned int i = 0; i < lv->get.lrv_num_keys_out; i++)
    {
        lv->lrv_value_idx_in = i;
        rc = lreg_node_exec_lrn_cb(LREG_NODE_CB_OP_READ_VAL, lrn, lv);
        if (rc)
            return rc;

        else if (!strncmp(key_name, LREG_VALUE_TO_KEY_STR(lv),
                          MIN(key_name_len, LREG_VALUE_STRING_MAX)))
            return 0;
    }

    return -ENOENT;
}

void
lreg_set_thread_ctx(pthread_t pthread_id);

bool
lreg_thread_ctx(void);

lreg_install_int_ctx_t
lreg_node_wait_for_completion(const struct lreg_node *lrn, bool install);

bool
lreg_install_has_queued_nodes(void);

int
lreg_notify(void);

int
lreg_remove_event_src(void);

int
lreg_util_processor(void);

int
lreg_get_eventfd(void);

int
lreg_node_wait_for_install_state(const struct lreg_node *lrn,
                                 enum lreg_node_states state);

#endif //_REGISTRY_H
