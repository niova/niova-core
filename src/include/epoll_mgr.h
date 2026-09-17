/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2019
 */

#ifndef _EPOLL_HANDLE_
#define _EPOLL_HANDLE_ 1

#include <sys/epoll.h>
#include <pthread.h>

#include "atomic.h"
#include "thread.h"

/**
 * There should be 1 event per fd (I think, based on my reading of the kernel's
 * eventpoll.c).  At this time, this library only supports these 3 static
 * values.
 */
#define EPOLL_MGR_MIN_EVENTS 32
#define EPOLL_MGR_DEF_EVENTS 128
#define EPOLL_MGR_MAX_EVENTS 1024

enum epoll_handle_ref_op
{
    EPH_REF_GET,
    EPH_REF_PUT
};

struct epoll_handle;
typedef void (*epoll_mgr_cb_t)(const struct epoll_handle *, uint32_t);
typedef void (*epoll_mgr_ref_cb_t)(void *, enum epoll_handle_ref_op);
typedef void (*epoll_mgr_ctx_op_cb_t)(void *);

struct epoll_handle
{
    int                   eph_fd;
    int                   eph_events;
    unsigned int          eph_installed     : 1;
    unsigned int          eph_installing    : 1;
    unsigned int          eph_destroying    : 1;
    unsigned int          eph_async_destroy : 1;
    void                 *eph_arg;
    epoll_mgr_cb_t        eph_cb;
    epoll_mgr_ref_cb_t    eph_ref_cb;
    epoll_mgr_ctx_op_cb_t eph_ctx_cb;
    CIRCLEQ_ENTRY(epoll_handle) eph_lentry;
    SLIST_ENTRY(epoll_handle) eph_cb_lentry;
};

CIRCLEQ_HEAD(epoll_handle_list, epoll_handle);
SLIST_HEAD(epoll_ctx_callback_list, epoll_handle);

typedef void epoll_mgr_cb_ctx_t;

struct epoll_mgr
{
    pthread_t                      epm_thread_id;
    pthread_mutex_t                epm_mutex;
    int                            epm_num_handles;
    int                            epm_epfd;
    struct epoll_handle            epm_wake_handle;
    unsigned int                   epm_ready : 1;
    niova_atomic64_t               epm_epoll_wait_cnt;
    struct epoll_handle_list       epm_active_list;
    struct epoll_handle_list       epm_destroy_list;
    struct epoll_ctx_callback_list epm_ctx_cb_list;
    int                            epm_ctx_cb_num;
    // eph whose ctx cb is executing; mgr thread only, no lock
    struct epoll_handle           *epm_ctx_cb_running;
};

struct niova_env_var;

void
epoll_mgr_env_var_cb(const struct niova_env_var *nev);

int
epoll_mgr_setup(struct epoll_mgr *epm);

int
epoll_mgr_close(struct epoll_mgr *epm);

int
epoll_handle_init(struct epoll_handle *eph, int fd, int events,
                  epoll_mgr_cb_t cb, void *arg,
                  void (*ref_cb)(void *, enum epoll_handle_ref_op));

int
epoll_handle_add(struct epoll_mgr *epm, struct epoll_handle *eph);

int
epoll_handle_mod(struct epoll_mgr *epm, struct epoll_handle *eph);

int
epoll_handle_del(struct epoll_mgr *epm, struct epoll_handle *eph);

int
epoll_handle_del_wait(struct epoll_mgr *epm, struct epoll_handle *eph);

int
epoll_mgr_wait_and_process_events(struct epoll_mgr *epm, int timeout);

int
epoll_mgr_ctx_cb_add(struct epoll_mgr *epm, struct epoll_handle *eph,
                     epoll_mgr_ctx_op_cb_t cb);

static inline bool
epoll_mgr_is_ready(const struct epoll_mgr *epm)
{
    return (epm && epm->epm_ready) ? true : false;
}

static inline bool
epoll_handle_is_installed(const struct epoll_handle *eph)
{
    return (eph && eph->eph_installed) ? true : false;
}

static inline bool
epoll_handle_releases_in_current_thread(const struct epoll_mgr *epm,
                                        const struct epoll_handle *eph)
{
    return (!eph->eph_ref_cb || epm->epm_thread_id == pthread_self()) ?
        true : false;
}

#endif
