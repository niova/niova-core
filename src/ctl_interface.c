/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2020
 */
#include <sys/inotify.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <limits.h>
#include <regex.h>
#include <dirent.h>
#include <utime.h>

#define _GNU_SOURCE
#include <pthread.h>

#include "ctl_interface.h"
#include "ctl_interface_cmd.h"
#include "env.h"
#include "file_util.h"
#include "log.h"
#include "registry.h"
#include "system_info.h"
#include "thread.h"
#include "util_thread.h"
#include "registry.h"

REGISTRY_ENTRY_FILE_GENERATE;
LREG_ROOT_ENTRY_GENERATE(ctlif_root_entry, LREG_USER_TYPE_CTL_INTERFACE);

#define DEFAULT_INOTIFY_PATH "/tmp/.niova"

#define INOTIFY_POLLING_MSEC 1000
#define INOTIFY_MAX_POLL_FDS 1

#define LCTLI_MAX 1
#define LCTLI_DEFAULT_IDX 0

typedef void lctli_inotify_thread_t;
typedef int  lctli_inotify_thread_int_t;

struct epoll_handle;

struct ctl_interface_op
{
    char            cio_input_name[NAME_MAX + 1];
    int             cio_status;
    bool            cio_init_ctx;
    size_t          cio_op_num;
    struct timespec cio_ts;
};

#define CTL_INTERFACE_COMPLETED_OP_CNT 32

struct ctl_interface
{
    const char              lctli_path[PATH_MAX + 1];
    bool                    lctli_init;
    int                     lctli_inotify_fd;
    int                     lctli_inotify_watch_fd;
    int                     lctli_input_dirfd;
    int                     lctli_output_dirfd;
    size_t                  lctli_op_cnt;
    struct epoll_handle    *lctli_eph;
    struct lreg_node        lctli_lreg;
    struct thread_ctl       lctli_thr_ctl;
    struct ctl_interface_op lctli_cio[CTL_INTERFACE_COMPLETED_OP_CNT];
};

enum lctli_subdirs
{
    LCTLI_SUBDIR_INIT,   // processed only during startup
    LCTLI_SUBDIR_INPUT,  // processed during runtime
    LCTLI_SUBDIR_OUTPUT,
    LCTLI_SUBDIR_MAX,
};

const char *lctliSubdirs[LCTLI_SUBDIR_MAX] =
{
    [LCTLI_SUBDIR_INIT] = "init",
    [LCTLI_SUBDIR_INPUT] = "input",
    [LCTLI_SUBDIR_OUTPUT] = "output"
};

enum lctli_reg_keys
{
    CTL_IF_REG_KEY_PATH,
    CTL_IF_REG_KEY_OP_CNT,
    CTL_IF_REG_KEY_OP_HISTORY,
    CTL_IF_REG_KEY__MAX,
};

enum ctl_interface_op_reg_keys
{
    CIO_REG_KEY_INPUT_NAME,
    CIO_REG_KEY_OP_NUM,
    CIO_REG_KEY_STATUS,
    CIO_REG_KEY_HUMAN_TIME,
    CIO_REG_KEY__MAX,
};

static struct ctl_interface localCtlIf[LCTLI_MAX];
static pthread_mutex_t lctlMutex = PTHREAD_MUTEX_INITIALIZER;
static int numLocalCtlIfs;

const char *
lctli_get_path(void)
{
    return localCtlIf[LCTLI_DEFAULT_IDX].lctli_path;
}

const char *
lctli_get_base_path(void)
{
    const struct niova_env_var *base_ev =
        env_get(NIOVA_ENV_VAR_inotify_base_path);
    if (base_ev && base_ev->nev_present)
        return base_ev->nev_string;

    return DEFAULT_INOTIFY_PATH;
}

int
lctli_get_inotify_fd(void)
{
    return localCtlIf[LCTLI_DEFAULT_IDX].lctli_inotify_fd;
}

static struct ctl_interface *
lctli_new(void)
{
    struct ctl_interface *new_lctli = NULL;

    pthread_mutex_lock(&lctlMutex);

    if (numLocalCtlIfs < LCTLI_MAX)
        new_lctli = &localCtlIf[numLocalCtlIfs++];

    pthread_mutex_unlock(&lctlMutex);

    return new_lctli;
}

static util_thread_ctx_t
lctli_store_completed_op(struct ctl_interface *lctli,
                         const struct ctli_cmd_handle *cch, int status)
{
    if (!lctli || !cch)
        return;

    const size_t op_num = lctli->lctli_op_cnt++;
    const size_t idx = op_num % CTL_INTERFACE_COMPLETED_OP_CNT;

    struct ctl_interface_op *cio = &lctli->lctli_cio[idx];

    cio->cio_status = status;
    cio->cio_init_ctx = init_ctx();
    cio->cio_op_num = op_num;
    niova_realtime_coarse_clock(&cio->cio_ts);

    strncpy(cio->cio_input_name, cch->ctlih_input_file_name, NAME_MAX);
}

static util_thread_ctx_t // or init_ctx_t
lctli_process_request(struct ctl_interface *lctli,
                      const struct ctli_cmd_handle *cch)
{
    int rc = ctlic_process_request(cch);

    lctli_store_completed_op(lctli, cch, rc);
}

static void
lctli_inotify_thread_poll_parse_buffer(struct ctl_interface *lctli,
                                       char *buf, const ssize_t len)
{
    const struct inotify_event *event;

    for (char *ptr = buf; ptr < (buf + len);
         ptr += sizeof(struct inotify_event) + event->len)
    {
        event = (const struct inotify_event *)ptr;

        LOG_MSG(LL_DEBUG, "event@%p mask=%x name=%s %s ",
                event, event->mask, event->name,
                (event->mask & IN_ISDIR) ? "[dir]" : "[file]");

        if (!(event->mask & IN_ISDIR))
        {
            struct ctli_cmd_handle cch = {
                .ctlih_reg_user_type = LREG_USER_TYPE_ANY,
                .ctlih_input_dirfd = lctli->lctli_input_dirfd,
                .ctlih_output_dirfd = lctli->lctli_output_dirfd,
                .ctlih_input_file_name = event->name
            };

            lctli_process_request(lctli, &cch);
        }
    }
}

void
lctli_inotify_parse_buffer(char *buf, size_t len, void *arg)
{
    struct ctl_interface *lctli =
        (struct ctl_interface *)(arg ? arg : &localCtlIf[LCTLI_DEFAULT_IDX]);

    if (lctli != &localCtlIf[LCTLI_DEFAULT_IDX])
        return;

    lctli_inotify_thread_poll_parse_buffer(lctli, buf, len);
}

int
lctli_get_default_output_dirfd(void)
{
    return localCtlIf[LCTLI_DEFAULT_IDX].lctli_output_dirfd;
}

static util_thread_ctx_t
lctli_inotify_thread_poll_handle_event(struct ctl_interface *lctli)
{
    char buf[INOTIFY_BUFFER_SIZE]
    __attribute__ ((aligned(__alignof__(struct inotify_event))));

    for (;;)
    {
        ssize_t len = read(lctli->lctli_inotify_fd, &buf, INOTIFY_BUFFER_SIZE);

        if (len < 0 && errno != EAGAIN)
        {
            len = errno;
            FATAL_MSG("read(): %s", strerror(len));
        }
        else if (len <= 0)
        {
            break;
        }
        else
        {
            lctli_inotify_thread_poll_parse_buffer(lctli, buf, len);
        }
    }
}

static util_thread_ctx_t
lctli_epoll_mgr_cb(const struct epoll_handle *eph, uint32_t events)
{
    NIOVA_ASSERT(eph);

    (void)events;

    struct ctl_interface *lctli = eph->eph_arg;

    if (eph->eph_fd != lctli->lctli_inotify_fd)
    {
        LOG_MSG(LL_ERROR, "invalid fd=%d (expected %d)",
                eph->eph_fd, lctli->lctli_inotify_fd);

        return;
    }

    return lctli_inotify_thread_poll_handle_event(lctli);
}

static int
lctli_remove_subdirs(struct ctl_interface *lctli)
{
    int err = 0;

    for (int i = 0; i < LCTLI_SUBDIR_MAX; i++)
    {
        char subdir_path[PATH_MAX];

        int rc = snprintf(subdir_path, PATH_MAX, "%s/%s",
                          lctli->lctli_path, lctliSubdirs[i]);

        if (rc >= PATH_MAX)
            return -ENAMETOOLONG;

        else if (rc < 0)
            return rc;

        rc = rmdir(subdir_path);
        SIMPLE_LOG_MSG(LL_DEBUG, "rmdir() %s: %s", subdir_path,
                       rc ? strerror(errno) : "Success");

        if (rc && !err)
            err = -errno;
    }

    if (!err)
    {
        err = rmdir(lctli->lctli_path);
        if (err)
            err = -errno;

        SIMPLE_LOG_MSG(LL_DEBUG, "rmdir() %s: %s", lctli->lctli_path,
                       err ? strerror(err) : "Success");
    }

    return err;
}

static int
lctli_prepare(struct ctl_interface *lctli)
{
    if (!lctli)
        return -EINVAL;

    else if (lctli->lctli_init)
        return -EALREADY;

    int rc = file_util_pathname_build(lctli->lctli_path);
    if (rc)
        return rc;

    time_t now = niova_realtime_coarse_clock_get_sec();

    struct utimbuf x = {
        .modtime = now,
        .actime = now,
    };

    // Notify niova-lookout that the process has returned
    rc = utime(lctli->lctli_path, &x);
    if (rc)
        SIMPLE_LOG_MSG(LL_WARN, "utimes(%s): %s",
                       lctli->lctli_path, strerror(errno));

    for (int i = 0; i < LCTLI_SUBDIR_MAX; i++)
    {
        char subdir_path[PATH_MAX];

        int rc = snprintf(subdir_path, PATH_MAX, "%s/%s",
                          lctli->lctli_path, lctliSubdirs[i]);

        if (rc >= PATH_MAX)
            return -ENAMETOOLONG;

        else if (rc < 0)
            return rc;

        rc = file_util_pathname_build(subdir_path);
        if (rc)
            return rc;

        if (i == LCTLI_SUBDIR_INPUT)
        {
            lctli->lctli_input_dirfd =
                open(subdir_path, O_DIRECTORY | O_RDONLY);

            if (lctli->lctli_input_dirfd < 0)
                return -errno;
        }
        else if (i == LCTLI_SUBDIR_OUTPUT)
        {
            lctli->lctli_output_dirfd =
                open(subdir_path, O_DIRECTORY | O_RDONLY);

            if (lctli->lctli_output_dirfd < 0)
                return -errno;
        }
    }

    lctli->lctli_inotify_fd = inotify_init1(IN_NONBLOCK);

    if (lctli->lctli_inotify_fd < 0)
    {
        int save_err = errno;

        LOG_MSG(LL_ERROR, "inotify_init1(): %s", strerror(save_err));

        return -save_err;
    }

    char input_path[PATH_MAX];

    rc = snprintf(input_path, PATH_MAX, "%s/%s",
                  lctli->lctli_path, lctliSubdirs[LCTLI_SUBDIR_INPUT]);
    if (rc >= PATH_MAX)
        return -ENAMETOOLONG;

    // note that IN_ATTRIB causes duplicate events on EXDEV file moves
    lctli->lctli_inotify_watch_fd =
        inotify_add_watch(lctli->lctli_inotify_fd, input_path,
                          IN_CLOSE_WRITE | IN_MOVED_TO);

    if (lctli->lctli_inotify_watch_fd < 0)
    {
        int save_err = errno;

        LOG_MSG(LL_ERROR, "inotify_add_watch(): %s", strerror(save_err));

        close(lctli->lctli_inotify_fd);
        return -save_err;
    }

    lctli->lctli_init = true;

    LOG_MSG(LL_DEBUG, "path=%s", lctli->lctli_path);

    return 0;
}

static int
lctli_setup_inotify_path(struct ctl_interface *lctli)
{
    // Presence of the env variable overrides all
    const struct niova_env_var *full_path_ev =
        env_get(NIOVA_ENV_VAR_inotify_path);

    if (full_path_ev && full_path_ev->nev_present)
    {
        strncpy((char *)lctli->lctli_path, full_path_ev->nev_string, PATH_MAX);
        return 0;
    }

    const struct niova_env_var *base_path_ev =
        env_get(NIOVA_ENV_VAR_inotify_base_path);

    const char *base_path =
        (base_path_ev && base_path_ev->nev_present) ?
        base_path_ev->nev_string : DEFAULT_INOTIFY_PATH;

    if (system_info_uuid_is_present())
    {
        uuid_t sys_uuid;
        system_info_get_uuid(sys_uuid);

        DECLARE_AND_INIT_UUID_STR(sys_uuid_str, sys_uuid);

        int rc = snprintf((char *)lctli->lctli_path, PATH_MAX, "/%s/%s/",
                          base_path, sys_uuid_str);
        if (rc < PATH_MAX)
            return 0;
    }

    int rc = snprintf((char *)lctli->lctli_path, PATH_MAX, "/%s/%d/",
                      base_path, getpid());
    if (rc >= PATH_MAX)
        return -ENAMETOOLONG;

    LOG_MSG(LL_NOTIFY, "path defaulting to %s", lctli->lctli_path);

    return 0;
}

static init_ctx_int_t
lctli_process_init_subdir(struct ctl_interface *lctli,
                          enum lreg_user_types reg_type)
{
    if (!lctli)
        return -EINVAL;

    char subdir_path[PATH_MAX] = {0};

    const struct niova_env_var *ev =
        env_get(NIOVA_ENV_VAR_ctl_interface_init_path);

    int rc = (ev && ev->nev_present) ?
        snprintf(subdir_path, PATH_MAX, "%s", ev->nev_string) :
        snprintf(subdir_path, PATH_MAX, "%s/%s",
                 lctli->lctli_path, lctliSubdirs[LCTLI_SUBDIR_INIT]);

    if (rc >= PATH_MAX)
        return -ENAMETOOLONG;

    int init_subdir_fd = open(subdir_path, O_RDONLY | O_DIRECTORY);
    if (init_subdir_fd < 0)
        return -errno;

    int close_rc = 0;

    DIR *init_subdir = fdopendir(init_subdir_fd);
    if (!init_subdir)
    {
        rc = -errno;
        close_rc = close(init_subdir_fd);
        if (close_rc)
            SIMPLE_LOG_MSG(LL_ERROR, "close():  %s", strerror(errno));

        return rc;
    }

    for (struct dirent *dent = readdir(init_subdir); dent != NULL;
         dent = readdir(init_subdir))
    {
        struct stat stb;

        int rc =
            fstatat(init_subdir_fd, dent->d_name, &stb, AT_SYMLINK_NOFOLLOW);
        if (rc || !S_ISREG(stb.st_mode))
        {
            SIMPLE_LOG_MSG(LL_NOTIFY, "bypass dentry=%s", dent->d_name);
            continue;
        }

        SIMPLE_LOG_MSG(LL_DEBUG, "processing dentry=%s type=%d",
                       dent->d_name, reg_type);

        struct ctli_cmd_handle cch = {
            .ctlih_input_dirfd = init_subdir_fd,
            .ctlih_output_dirfd = lctli->lctli_output_dirfd,
            .ctlih_input_file_name = dent->d_name,
            .ctlih_reg_user_type = reg_type,
        };

        lctli_process_request(lctli, &cch);
    }

    close_rc = closedir(init_subdir);
    if (close_rc)
        SIMPLE_LOG_MSG(LL_ERROR, "closedir():  %s", strerror(errno));

    return close_rc;
}

static util_thread_ctx_reg_int_t
lctli_lreg_op_history_lreg_cb(enum lreg_node_cb_ops op, struct lreg_node *lrn,
                              struct lreg_value *lv)
{
    if (!lrn || !lrn->lrn_cb_arg)
        return -EINVAL;

    const struct ctl_interface *lctli =
        (const struct ctl_interface *)lrn->lrn_cb_arg;

    if (lv)
        lv->get.lrv_num_keys_out = CIO_REG_KEY__MAX;

    NIOVA_ASSERT(lrn->lrn_vnode_child);
    struct lreg_vnode_data *vd = &lrn->lrn_lvd;

    if (vd->lvd_index >= CTL_INTERFACE_COMPLETED_OP_CNT ||
        vd->lvd_index >= lctli->lctli_op_cnt)
	return -ERANGE;

    const size_t base_idx =
        lctli->lctli_op_cnt >= CTL_INTERFACE_COMPLETED_OP_CNT
        ? (lctli->lctli_op_cnt) % CTL_INTERFACE_COMPLETED_OP_CNT
        : 0;

    size_t idx = (base_idx + vd->lvd_index) % CTL_INTERFACE_COMPLETED_OP_CNT;

    SIMPLE_LOG_MSG(LL_DEBUG, "vd-idx=%u base=%zu idx=%zu opcnt=%zu",
                   vd->lvd_index, base_idx, idx, lctli->lctli_op_cnt);

    const struct ctl_interface_op *cio = &lctli->lctli_cio[idx];

    switch (op)
    {
    case LREG_NODE_CB_OP_GET_NAME:
        strncpy(lv->lrv_key_string, "ctl-if-ops", LREG_VALUE_STRING_MAX);
        strncpy(LREG_VALUE_TO_OUT_STR(lv), "none", LREG_VALUE_STRING_MAX);
        break;

    case LREG_NODE_CB_OP_READ_VAL:
        switch (lv->lrv_value_idx_in)
        {
        case CIO_REG_KEY_INPUT_NAME:
            lreg_value_fill_string(lv, "input-file", cio->cio_input_name);
            break;
        case CIO_REG_KEY_OP_NUM:
            lreg_value_fill_unsigned(lv, "op-num", cio->cio_op_num);
            break;
        case CIO_REG_KEY_STATUS:
            lreg_value_fill_string(lv, "status", strerror(-cio->cio_status));
            break;
        case CIO_REG_KEY_HUMAN_TIME:
            lreg_value_fill_string_time(lv, "time", cio->cio_ts.tv_sec);
            break;
        default:
            return -EOPNOTSUPP;
        }

    case LREG_NODE_CB_OP_INSTALL_NODE: // fall through
    case LREG_NODE_CB_OP_DESTROY_NODE: // fall through
    case LREG_NODE_CB_OP_INSTALL_QUEUED_NODE:
	break;

    default:
        return -EOPNOTSUPP;
    }

    return 0;
}

static util_thread_ctx_reg_int_t
lctli_lreg_cb(enum lreg_node_cb_ops op, struct lreg_node *lrn,
              struct lreg_value *lrv)
{
    if (!lrn || (!lrv && (op == LREG_NODE_CB_OP_GET_NODE_INFO ||
                          op == LREG_NODE_CB_OP_READ_VAL ||
                          op == LREG_NODE_CB_OP_WRITE_VAL)))
	return -EINVAL;

    struct ctl_interface *lctli = (struct ctl_interface *)lrn->lrn_cb_arg;
    NIOVA_ASSERT(lctli == OFFSET_CAST(ctl_interface, lctli_lreg, lrn));

    size_t op_hist_sz = CTL_INTERFACE_COMPLETED_OP_CNT;

    switch (op)
    {
    case LREG_NODE_CB_OP_INSTALL_NODE: /* fall through */
    case LREG_NODE_CB_OP_DESTROY_NODE: /* fall through */
    case LREG_NODE_CB_OP_INSTALL_QUEUED_NODE:
        break; // No-ops since these entries are effectively static
    case LREG_NODE_CB_OP_GET_NODE_INFO:
        lrv->get.lrv_num_keys_out = CTL_IF_REG_KEY__MAX;
        snprintf(lrv->lrv_key_string, LREG_VALUE_STRING_MAX, "ctl_interface");
        break;
    case LREG_NODE_CB_OP_READ_VAL:
	switch (lrv->lrv_value_idx_in)
        {
        case CTL_IF_REG_KEY_PATH:
             lreg_value_fill_string(lrv, "path", lctli->lctli_path);
             break;
        case CTL_IF_REG_KEY_OP_CNT:
            lreg_value_fill_unsigned(lrv, "op-cnt", lctli->lctli_op_cnt);
            break;
        case CTL_IF_REG_KEY_OP_HISTORY:
            if (lctli->lctli_op_cnt < CTL_INTERFACE_COMPLETED_OP_CNT)
                op_hist_sz = lctli->lctli_op_cnt;

            lreg_value_fill_varray(lrv, "recent-ops",
                                   LREG_USER_TYPE_CTL_INTERFACE_ROP,
                                   op_hist_sz, lctli_lreg_op_history_lreg_cb);
            break;
        default:
            return -EOPNOTSUPP;
        }
        break;
    default:
        return -EOPNOTSUPP;
    }

    return 0;
}

/**
 * lctli_util_thread_unregister - uninstall the ctl interface from the utility
 *    thread.  This removes both 'sides' of the interface - meaning the user
 *    side, which communicates using inotify, as well as the lreg handling.
 *    Once this function has successfully completed, neither user inputs nor
 *    lreg_node install / removals will be processed until new handlers are
 *    assigned.
 */
int
lctli_util_thread_unregister(void)
{
    if (numLocalCtlIfs == 0)
        return -EAGAIN;

    if (numLocalCtlIfs != 1) // we only support on ctl-interface at this time
        return -ERANGE;

    struct ctl_interface *lctli = &localCtlIf[numLocalCtlIfs - 1];
    if (lctli->lctli_eph == NULL)
        return -EINVAL;

    lreg_notify();

    int rc = util_thread_remove_event_src(lctli->lctli_eph);
    if (rc)
        return rc;

    rc = lreg_remove_event_src();
    if (rc)
        return rc;

    return 0;
}

static init_ctx_t NIOVA_CONSTRUCTOR(LCTLI_SUBSYS_CTOR_PRIORITY)
lctli_subsystem_init(void)
{
    struct ctl_interface *lctli = lctli_new();

    NIOVA_ASSERT(lctli);
    NIOVA_ASSERT(numLocalCtlIfs == 1);

    LREG_ROOT_ENTRY_INSTALL(ctlif_root_entry);

    /* offsetof() could be used inside the cb to calculate 'lctli'.  However,
     * a VARRAY is used in lctli_lreg_cb and the varray cb will not be
     * presented with lctli_lreg so we must set the cb_arg.
     */
    lreg_node_init(&lctli->lctli_lreg, LREG_USER_TYPE_CTL_INTERFACE,
                   lctli_lreg_cb, lctli, LREG_INIT_OPT_REVERSE_VARRAY);

    int rc = lreg_node_install(&lctli->lctli_lreg,
                               LREG_ROOT_ENTRY_PTR(ctlif_root_entry));
    FATAL_IF(rc, "lreg_node_install() %s", strerror(-rc));

    rc = lctli_setup_inotify_path(lctli);
    FATAL_IF(rc, "lctli_setup_inotify_path(): %s", strerror(-rc));

    rc = lctli_prepare(lctli);
    FATAL_IF(rc, "lctli_prepare(): %s (path=%s)",
             strerror(-rc), lctli->lctli_path);

    lctli->lctli_eph = NULL;
    rc = util_thread_install_event_src(lctli->lctli_inotify_fd, EPOLLIN,
                                       lctli_epoll_mgr_cb, (void *)lctli,
                                       &lctli->lctli_eph);

    FATAL_IF(rc, "util_thread_install_event_src(): %s", strerror(-rc));
    FATAL_IF((lctli->lctli_eph == NULL),
             "util_thread_install_event_src(): NULL eph pointer");
}

/**
 * lctli_init_subdir_rescan - this function is used by modules which have been
 *    started after init_ctx has completed.
 * @reg_type: the type of registry node to process.  Note that the special
 *    types, NONE and ANY, are not allowed since this function expects to
 *    process ctl-interface cmds for a specific module.
 */
int
lctli_init_subdir_rescan(enum lreg_user_types reg_type)
{
    if (reg_type == LREG_USER_TYPE_ANY || reg_type == LREG_USER_TYPE_NONE)
        return -EINVAL;

    struct ctl_interface *lctli = &localCtlIf[LCTLI_DEFAULT_IDX];

    int rc = lctli_process_init_subdir(lctli, reg_type);
    if (rc)
        LOG_MSG(LL_WARN, "lctli_process_init_subdir(): %s (path=%s)",
                strerror(-rc), lctli->lctli_path);

    return rc;
}

/**
 * lctli_subsystem_enable - this scans the init/ subdir towards the end of the
 *    init context.  This is to give an opportunity for subsystem to have
 *    installed their registry hooks before the init/ scan is run.
 */
static init_ctx_t NIOVA_CONSTRUCTOR(LCTLI_SUBSYS_ENABLE_CTOR_PRIORITY)
lctli_subsystem_enable(void)
{
    struct ctl_interface *lctli = &localCtlIf[LCTLI_DEFAULT_IDX];

    int rc = lctli_process_init_subdir(lctli, LREG_USER_TYPE_ANY);
    FATAL_IF(rc, "lctli_process_init_subdir(): %s (path=%s)",
             strerror(-rc), lctli->lctli_path);
}

static destroy_ctx_t NIOVA_DESTRUCTOR(LCTLI_SUBSYS_CTOR_PRIORITY)
lctli_subsystem_destroy(void)
{
    struct ctl_interface *lctli = &localCtlIf[LCTLI_DEFAULT_IDX];

    if (lctli->lctli_init)
    {
        //remove from utility thread?

        inotify_rm_watch(lctli->lctli_inotify_fd,
                         lctli->lctli_inotify_watch_fd);
    }

    if (!system_info_uuid_is_present())
        lctli_remove_subdirs(lctli);

    return;
}
