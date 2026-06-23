#include <sys/ioctl.h>

#include "alloc.h"
#include "log.h"
#include "epoll_mgr.h"
#include "io.h"
#include "tcp.h"
#include "tcp_mgr.h"
#include "util.h"

REGISTRY_ENTRY_FILE_GENERATE;

static int tcpWorkerCnt = TCP_MGR_NTHREADS;


int
tcp_mgr_worker_cnt_get(void)
{
    return tcpWorkerCnt;
}

void
tcp_mgr_credits_set(niova_atomic32_t *credits, uint32_t cnt)
{
    niova_atomic_init(credits, cnt);
}

static void *
tcp_mgr_credits_malloc(niova_atomic32_t *credits, size_t sz)
{
    int32_t new_credits = niova_atomic_dec(credits);
    SIMPLE_LOG_MSG(LL_TRACE, "malloc sz %lu, new cred %u", sz, new_credits);
    if (new_credits < 0)
    {
        niova_atomic_inc(credits);
        return NULL;
    }

    void *buf = niova_malloc_can_fail(sz);
    if (!buf)
        return NULL;

    return buf;
}

static void
tcp_mgr_credits_free(niova_atomic32_t *credits, void *buf)
{
    niova_free(buf);

    niova_atomic_inc(credits);
}

void
tcp_mgr_bulk_credits_set(struct tcp_mgr_instance *tmi, uint32_t cnt)
{
    tcp_mgr_credits_set(&tmi->tmi_bulk_credits, cnt);
}

void
tcp_mgr_incoming_credits_set(struct tcp_mgr_instance *tmi, uint32_t cnt)
{
    tcp_mgr_credits_set(&tmi->tmi_incoming_credits, cnt);
}

static void *
tcp_mgr_bulk_malloc(struct tcp_mgr_instance *tmi, size_t sz)
{
    return tcp_mgr_credits_malloc(&tmi->tmi_bulk_credits, sz);
}

static void
tcp_mgr_bulk_free(struct tcp_mgr_instance *tmi, void *buf)
{
    tcp_mgr_credits_free(&tmi->tmi_bulk_credits, buf);
}

static void
tcp_mgr_connection_put(struct tcp_mgr_connection *tmc)
{
    if (tmc->tmc_tmi && tmc->tmc_tmi->tmi_connection_ref_cb)
        tmc->tmc_tmi->tmi_connection_ref_cb(tmc, EPH_REF_PUT);
}

static void
tcp_mgr_conn_recv_inline(struct tcp_mgr_connection *tmc);

static void
tcp_mgr_conn_reenable(struct tcp_mgr_connection *tmc)
{
    if (!tmc->tmc_handoff)
        return;

    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;
    struct tcp_mgr_connq *tmcq = &tmi->tmi_connq;

    niova_mutex_lock(&tmcq->tmcq_mutex);

    if (!tmc->tmc_handoff)
    {
        niova_mutex_unlock(&tmcq->tmcq_mutex);
        return;
    }

    tmc->tmc_handoff = 0; // mark the tmc as not residing on the queue

    int rc = epoll_handle_mod(tmi->tmi_epoll_mgr, &tmc->tmc_eph);
    if (rc != 0)
        LOG_MSG(LL_DEBUG, "epoll_handle_mod(): %s", strerror(-rc));

    niova_mutex_unlock(&tmcq->tmcq_mutex);
}

static void *
tcp_mgr_worker(void *arg)
{
    struct thread_ctl *tc = (struct thread_ctl *)arg;
    NIOVA_ASSERT(tc && tc->tc_arg);

    struct tcp_mgr_instance *tmi = (struct tcp_mgr_instance *)tc->tc_arg;
    struct tcp_mgr_connq *tmcq = &tmi->tmi_connq;

    THREAD_LOOP_WITH_CTL(tc)
    {
        struct tcp_mgr_connection *tmc = NULL;

        NIOVA_WAIT_COND(
            (!STAILQ_EMPTY(&tmcq->tmcq_queue) ||
             thread_ctl_has_flag(tc, TC_FLAG_HALT)),
             &tmcq->tmcq_mutex, &tmcq->tmcq_cond,
            {
                if (!STAILQ_EMPTY(&tmcq->tmcq_queue) &&
                    !thread_ctl_has_flag(tc, TC_FLAG_HALT))
                {
                    tmc = STAILQ_FIRST(&tmcq->tmcq_queue);
                    STAILQ_REMOVE_HEAD(&tmcq->tmcq_queue, tmc_lentry);
                }
            });

        SIMPLE_LOG_MSG(LL_DEBUG, "tmc=%p", tmc);

        if (tmc == NULL)
            continue;

        NIOVA_ASSERT(tmc->tmc_handoff);

        /* Start work.  Note: this method ensures that only a single operation,
         * per connection, will be processed at any one time.  This ensures that
         * a greedy client will not occupy all processing threads.  If this
         * behavior were to be changed, it's likely that a per-connection send
         * mutex would be required to prevent interleaving of reply contents on
         * the socket.
         * NOTE that the send mutex may be needed anyway since the write replies
         * will be handled async - by another thread.  The other issue becomes
         * socket writes which can't be completed, in this case we need to
         * queue the sends on the connection, similar to how nconn.c works.
         */
        tcp_mgr_conn_recv_inline(tmc);
        // end work

        tcp_mgr_conn_reenable(tmc);
    }
    return NULL;
}

env_cb_ctx_t
tcp_mgr_set_thread_cnt_env_cb(const struct niova_env_var *ev)
{
    if (!ev)
        return;

    tcpWorkerCnt = ev->nev_long_value;
}

void
tcp_mgr_destroy(struct tcp_mgr_instance *tmi)
{
    if (!tmi)
        return;

    // Check if workers exist by checking thread_ids
    bool has_workers = false;
    for (size_t i = 0; i < TCP_MGR_NTHREADS; i++)
    {
        if (tmi->tmi_workers[i].tc_thread_id != 0)
        {
            has_workers = true;
            break;
        }
    }

    if (!has_workers)
        return;

    struct tcp_mgr_connq *tmcq = &tmi->tmi_connq;

    // Wake up all waiting workers by broadcasting on the condition variable
    // This ensures they wake up and see the HALT flag when we call thread_halt_and_destroy()
    // Try to lock the mutex first - if we can't, workers might be using it
    int trylock_rc = pthread_mutex_trylock(&tmcq->tmcq_mutex);
    if (trylock_rc == 0)
    {
        pthread_cond_broadcast(&tmcq->tmcq_cond);
        pthread_mutex_unlock(&tmcq->tmcq_mutex);
    }
    else if (trylock_rc != EINVAL)
    {
        // Mutex is locked - broadcast anyway (pthread_cond_broadcast doesn't require mutex)
        pthread_cond_broadcast(&tmcq->tmcq_cond);
    }
    // If EINVAL, mutex not initialized (zeroed), skip broadcast

    // Halt and destroy each worker thread
    // thread_halt_and_destroy() sets HALT flag and pthread_join()s to wait for exit
    for (size_t i = 0; i < TCP_MGR_NTHREADS; i++)
    {
        if (tmi->tmi_workers[i].tc_thread_id != 0)
        {
            thread_halt_and_destroy(&tmi->tmi_workers[i]);
        }
    }

    tmi->tmi_nworkers = 0;
}

int
tcp_mgr_setup(struct tcp_mgr_instance *tmi, void *data,
              epoll_mgr_ref_cb_t connection_ref_cb,
              tcp_mgr_recv_cb_t recv_cb,
              tcp_mgr_bulk_size_cb_t bulk_size_cb,
              tcp_mgr_handshake_cb_t handshake_cb,
              tcp_mgr_handshake_fill_t handshake_fill,
              size_t handshake_size, uint32_t bulk_credits,
              uint32_t incoming_credits, bool conn_recv_handoff)
{
    NIOVA_ASSERT(tmi);

    // Check if workers already exist - if so, destroy them first
    // This handles the case where tcp_mgr_setup is called multiple times
    // without going through raft_server_instance_init (which zeros everything).
    // Note: After bulk recovery, raft_server_instance_init zeros the entire
    // raft_instance including tcp_mgr_instance, so workers should already be gone.
    if (tmi->tmi_nworkers > 0)
    {
        tcp_mgr_destroy(tmi);
    }
    else
    {
        // Double-check for leftover thread_ids (shouldn't happen after memset, but be safe)
        for (size_t i = 0; i < TCP_MGR_NTHREADS; i++)
        {
            if (tmi->tmi_workers[i].tc_thread_id != 0)
            {
                tcp_mgr_destroy(tmi);
                break;
            }
        }
    }

    tmi->tmi_data = data;
    tmi->tmi_nworkers = 0;
    tmi->tmi_connection_ref_cb = connection_ref_cb;
    tmi->tmi_recv_cb = recv_cb;
    tmi->tmi_bulk_size_cb = bulk_size_cb;
    tmi->tmi_handshake_cb = handshake_cb;
    tmi->tmi_handshake_fill = handshake_fill;
    tmi->tmi_handshake_size = handshake_size;
    tmi->tmi_conn_recv_handoff = !!conn_recv_handoff;

    tcp_mgr_bulk_credits_set(tmi, bulk_credits);
    tcp_mgr_incoming_credits_set(tmi, incoming_credits);

    pthread_mutex_init(&tmi->tmi_epoll_ctx_mutex, NULL);

    struct tcp_mgr_connq *tmcq = &tmi->tmi_connq;
    STAILQ_INIT(&tmcq->tmcq_queue);
    pthread_mutex_init(&tmcq->tmcq_mutex, NULL);
    pthread_cond_init(&tmcq->tmcq_cond, NULL);

    if (conn_recv_handoff)
    {
        int rc = 0;

        SIMPLE_LOG_MSG(LL_DEBUG, "Number of worker threads: %d", tcpWorkerCnt);
        for (int i = 0; i < tcpWorkerCnt; i++)
        {
            char thr_name[MAX_THREAD_NAME] = {0};
            snprintf(thr_name, MAX_THREAD_NAME, "tcp_wrk.%d", i);

            rc = thread_create(tcp_mgr_worker, &tmi->tmi_workers[i], thr_name,
                               (void *)tmi, NULL);
            if (rc == 0)
            {
                tmi->tmi_nworkers++;
                thread_ctl_run(&tmi->tmi_workers[i]);
            }
            else
            {
                break;
            }
        }

        if (tmi->tmi_nworkers == 0)
            return rc;

        for (size_t i = 0; i < tmi->tmi_nworkers; i++)
            thread_creator_wait_until_ctl_loop_reached(&tmi->tmi_workers[i]);
    }

    return 0;
}

int
tcp_mgr_sockets_setup(struct tcp_mgr_instance *tmi, const char *ipaddr,
                      int port)
{
    strncpy(tmi->tmi_listen_socket.tsh_ipaddr, ipaddr, IPV4_STRLEN);
    tmi->tmi_listen_socket.tsh_ipaddr[IPV4_STRLEN - 1] = 0;
    tmi->tmi_listen_socket.tsh_port = port;

    return tcp_socket_setup(&tmi->tmi_listen_socket);
}

int
tcp_mgr_sockets_close(struct tcp_mgr_instance *tmi)
{
    if (tmi->tmi_listen_eph.eph_installed)
        epoll_handle_del(tmi->tmi_epoll_mgr, &tmi->tmi_listen_eph);

    return tcp_socket_close(&tmi->tmi_listen_socket);
}

int
tcp_mgr_sockets_bind(struct tcp_mgr_instance *tmi)
{
    int rc = tcp_socket_bind(&tmi->tmi_listen_socket);
    if (rc)
        tcp_mgr_sockets_close(tmi);

    return rc;
}

static int
tcp_mgr_connection_setup_internal(struct tcp_mgr_connection *tmc,
                                  struct tcp_mgr_instance *tmi,
                                  void (*ref_cb)(void *,
                                                 enum epoll_handle_ref_op))
{
    SIMPLE_LOG_MSG(LL_TRACE, "tcp_mgr_connection_setup(): tmc %p", tmc);

    tcp_socket_handle_init(&tmc->tmc_tsh);

    int rc = epoll_handle_init(&tmc->tmc_eph, -1, 0, NULL, tmc, ref_cb);
    if (rc)
    {
        SIMPLE_LOG_MSG(LL_ERROR, "epoll_handle_init(): rc=%d", rc);
        return rc;
    }

    tmc->tmc_tmi = tmi;

    // setup during handshake
    tmc->tmc_header_size = 0;

    tmc->tmc_bulk_buf = NULL;
    tmc->tmc_bulk_offset = 0;
    tmc->tmc_bulk_remain = 0;

    tmc->tmc_epoll_ctx_cb = NULL;

    tmc->tmc_status = TMCS_DISCONNECTED;

    pthread_mutex_init(&tmc->tmc_send_mutex, NULL);

    return 0;
}

void
tcp_mgr_connection_setup(struct tcp_mgr_connection *tmc,
                         struct tcp_mgr_instance *tmi,
                         const char *ipaddr, int port)
{
    if (tmc->tmc_status != TMCS_NEEDS_SETUP)
    {
        DBG_TCP_MGR_CXN(LL_ERROR, tmc,
                        "setup called on existing conn, status=%d",
                        tmc->tmc_status);
        return;
    }
    int rc = tcp_mgr_connection_setup_internal(tmc, tmi,
                                               tmi->tmi_connection_ref_cb);
    if (rc)
        return;

    tcp_socket_handle_set_data(&tmc->tmc_tsh, ipaddr, port);
}

static void
tcp_mgr_connection_close_internal(struct tcp_mgr_connection *tmc)
{
    DBG_TCP_MGR_CXN(LL_TRACE, tmc, "closing");

    if (tmc->tmc_status != TMCS_CONNECTING && tmc->tmc_status != TMCS_CONNECTED)
        DBG_TCP_MGR_CXN(LL_NOTIFY, tmc, "unexpected status: %d",
                        tmc->tmc_status);

    tmc->tmc_status = TMCS_DISCONNECTING;
    if (tmc->tmc_eph.eph_installed)
        epoll_handle_del(tmc->tmc_tmi->tmi_epoll_mgr, &tmc->tmc_eph);

    if (tmc->tmc_bulk_buf)
        tcp_mgr_bulk_free(tmc->tmc_tmi, tmc->tmc_bulk_buf);

    tmc->tmc_bulk_buf = NULL;
    tmc->tmc_bulk_offset = 0;
    tmc->tmc_bulk_remain = 0;

    tcp_socket_close(&tmc->tmc_tsh);
    tmc->tmc_status = TMCS_DISCONNECTED;
}

/*
 * tcp_mgr_incoming_getput is required by epoll_mgr_ctx_cb_add. Incoming
 * connections are created and destroyed as part of connection workflow,
 * however, so ref counting is only used as a sanity check.
 */
static void
tcp_mgr_incoming_getput(void *tmc, enum epoll_handle_ref_op op)
{
    struct tcp_mgr_incoming_connection *incoming =
        OFFSET_CAST(tcp_mgr_incoming_connection, tmic_tmc, tmc);

    int newref = (op == EPH_REF_GET)
        ? niova_atomic_inc(&incoming->tmic_refcnt)
        : niova_atomic_dec(&incoming->tmic_refcnt);

    SIMPLE_LOG_MSG(LL_TRACE, "newref %d", newref);
    NIOVA_ASSERT(newref > 0);
}

static struct tcp_mgr_connection *
tcp_mgr_incoming_new(struct tcp_mgr_instance *tmi)
{
    struct tcp_mgr_incoming_connection *incoming =
        tcp_mgr_credits_malloc(&tmi->tmi_incoming_credits,
                               sizeof(struct tcp_mgr_incoming_connection));
    if (!incoming)
        return NULL;

    niova_atomic_init(&incoming->tmic_refcnt, 1);
    struct tcp_mgr_connection *tmc = &incoming->tmic_tmc;

    tmc->tmc_status = TMCS_NEEDS_SETUP;
    int rc = tcp_mgr_connection_setup_internal(tmc, tmi,
                                               tcp_mgr_incoming_getput);
    if (rc)
    {
        tcp_mgr_credits_free(&tmi->tmi_incoming_credits, incoming);
        return NULL;
    }

    tmc->tmc_status = TMCS_CONNECTING;

    return tmc;
}

static void
tcp_mgr_incoming_fini_err(struct tcp_mgr_connection *tmc)
{
    struct tcp_mgr_incoming_connection *incoming =
        OFFSET_CAST(tcp_mgr_incoming_connection, tmic_tmc, tmc);
    int refcnt = niova_atomic_read(&incoming->tmic_refcnt);
    NIOVA_ASSERT(refcnt == 1);

    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;
    tcp_mgr_connection_close_internal(tmc);
    tcp_mgr_credits_free(&tmi->tmi_incoming_credits, incoming);
}

static void
tcp_mgr_incoming_fini(struct tcp_mgr_connection *tmc)
{
    struct tcp_mgr_incoming_connection *incoming =
        OFFSET_CAST(tcp_mgr_incoming_connection, tmic_tmc, tmc);
    int refcnt = niova_atomic_read(&incoming->tmic_refcnt);
    NIOVA_ASSERT(refcnt == 1);

    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;
    tcp_mgr_credits_free(&tmi->tmi_incoming_credits, incoming);
}

static epoll_mgr_cb_ctx_t
tcp_mgr_epoll_ctx_cb(void *data)
{
    struct tcp_mgr_connection *tmc = data;
    if (tmc->tmc_epoll_ctx_cb)
        tmc->tmc_epoll_ctx_cb(tmc);

    tmc->tmc_epoll_ctx_cb = NULL;
}

static int
tcp_mgr_connection_epoll_ctx_run(struct tcp_mgr_connection *tmc,
                                 tcp_mgr_connection_epoll_ctx_cb_t cb)
{
    DBG_TCP_MGR_CXN(LL_TRACE, tmc, "");

    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;
    struct epoll_mgr *epm = tmi->tmi_epoll_mgr;
    if (!cb)
        return -EINVAL;

    niova_mutex_lock(&tmi->tmi_epoll_ctx_mutex);
    if (tmc->tmc_epoll_ctx_cb)
    {
        // can happen if other thread is about to call epoll_mgr_ctx_cb_add
        if (!tmc->tmc_eph.eph_ctx_cb)
            SIMPLE_LOG_MSG(LL_DEBUG, "mismatch between tmc and eph ctx cbs");
        niova_mutex_unlock(&tmi->tmi_epoll_ctx_mutex);
        return -EAGAIN;
    }
    else
    {
        tmc->tmc_epoll_ctx_cb = cb;
        niova_mutex_unlock(&tmi->tmi_epoll_ctx_mutex);
    }

    int rc = epoll_mgr_ctx_cb_add(epm, &tmc->tmc_eph, tcp_mgr_epoll_ctx_cb);
    if (rc)
    {
        tmc->tmc_epoll_ctx_cb = NULL;
        SIMPLE_LOG_MSG(LL_TRACE, "epoll_mgr_ctx_cb_add(): rc=%d eph=%p", rc,
                       &tmc->tmc_eph);
        LOG_MSG(LL_ERROR, "cannot start epoll connection task, rc: %d", rc);
    }

    return rc;
}

static int
tcp_mgr_connection_epoll_add(struct tcp_mgr_connection *tmc,
                             uint32_t events,
                             epoll_mgr_cb_t cb,
                             void (*ref_cb)(void *, enum epoll_handle_ref_op))
{
    DBG_TCP_MGR_CXN(LL_TRACE, tmc, "events=%d", events);

    int rc = epoll_handle_init(&tmc->tmc_eph, tmc->tmc_tsh.tsh_socket,
                               events, cb, tmc, ref_cb);
    if (rc)
        return rc;

    return epoll_handle_add(tmc->tmc_tmi->tmi_epoll_mgr, &tmc->tmc_eph);
}

static int
tcp_mgr_handshake_iov_init(struct tcp_mgr_instance *tmi, struct iovec *iov)
{
    iov->iov_base = niova_malloc_can_fail(tmi->tmi_handshake_size);
    if (!iov->iov_base)
        return -ENOMEM;

    iov->iov_len = tmi->tmi_handshake_size;

    return 0;
}

static void
tcp_mgr_handshake_iov_fini(struct iovec *iov)
{
    NIOVA_ASSERT(iov && iov->iov_base);
    niova_free(iov->iov_base);
}

static int
tcp_mgr_epoll_handle_rc_get(const struct epoll_handle *eph,
                            uint32_t events)
{
    int rc = 0;

    if (events & (EPOLLHUP | EPOLLERR))
    {
        socklen_t rc_len = sizeof(rc);
        int rc2 = getsockopt(eph->eph_fd, SOL_SOCKET, SO_ERROR, &rc, &rc_len);
        if (rc2)
        {
            SIMPLE_LOG_MSG(LL_ERROR, "Error getting socket error: %d", rc2);
            return rc2;
        }
        SIMPLE_LOG_MSG(LL_DEBUG, "ev=%d: received %s, rc=%d", events,
                       events & EPOLLHUP ? "HUP" : "ERR", rc);
    }

    return -rc;
}

static int
tcp_mgr_bulk_progress_recv(struct tcp_mgr_connection *tmc)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);

    if (!tmc->tmc_bulk_remain)
        return 0;

    char *bulk = tmc->tmc_bulk_buf + tmc->tmc_bulk_offset;

    struct iovec iov = {
        .iov_base = bulk,
        .iov_len = tmc->tmc_bulk_remain,
    };

    ssize_t recv_bytes = tcp_socket_recv(&tmc->tmc_tsh, &iov, 1, NULL, false);

    SIMPLE_LOG_MSG(LL_DEBUG, "recv_bytes=%ld iov_len=%ld", recv_bytes,
                   iov.iov_len);

    if (recv_bytes == -EAGAIN)
        recv_bytes = 0;
    else if (recv_bytes == 0)
        return -ENOTCONN;
    else if (recv_bytes < 0)
        return recv_bytes;

    NIOVA_ASSERT((size_t)recv_bytes <= tmc->tmc_bulk_remain);

    tmc->tmc_bulk_offset += recv_bytes;
    tmc->tmc_bulk_remain -= recv_bytes;

    return 0;
}

static int
tcp_mgr_bulk_prepare_and_recv(struct tcp_mgr_connection *tmc, size_t bulk_size,
                              void *hdr, size_t hdr_size)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);
    NIOVA_ASSERT(tmc && !tmc->tmc_bulk_buf && !tmc->tmc_bulk_remain);
    if (!bulk_size)
        return 0;

    size_t buf_size = hdr_size + bulk_size;
    if (buf_size > TCP_MGR_MAX_BULK_SIZE)
        return -E2BIG;

    // buffer free'd in tcp_mgr_recv_cb
    void *buf = tcp_mgr_bulk_malloc(tmc->tmc_tmi, buf_size);
    if (!buf)
    {
        DBG_TCP_MGR_CXN(LL_NOTIFY, tmc, "cannot allocate bulk buf");

        int bytes_avail;
        ioctl(tmc->tmc_tsh.tsh_socket, FIONREAD, &bytes_avail);
        if (bytes_avail >= (ssize_t)bulk_size)
            buf = niova_malloc_can_fail(buf_size);

        if (!buf)
            return -ENOMEM;
    }

    if (hdr && hdr_size)
        memcpy(buf, hdr, hdr_size);

    tmc->tmc_bulk_buf = buf;
    tmc->tmc_bulk_offset = hdr_size;
    tmc->tmc_bulk_remain = bulk_size;

    return 0;
}

static int
tcp_mgr_tmi_exec_recv_cb(struct tcp_mgr_connection *tmc, char *sink_buf,
                         size_t size)
{
    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;

    return tmi->tmi_recv_cb(tmc, sink_buf, size, tmi->tmi_data);
}

static int
tcp_mgr_new_msg_handler(struct tcp_mgr_connection *tmc)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);

    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;
    ssize_t header_size = tmc->tmc_header_size;

    NIOVA_ASSERT(tmi->tmi_recv_cb && tmi->tmi_bulk_size_cb && header_size &&
                 header_size <= TCP_MGR_MAX_HDR_SIZE);

    static __thread char sink_buf[TCP_MGR_MAX_HDR_SIZE];
    struct iovec iov;

    // Note that tcp_socket_recv_all() will modify the iov
    iov.iov_base = sink_buf;
    iov.iov_len = header_size;

    ssize_t rc = tcp_socket_recv_all(&tmc->tmc_tsh, &iov, NULL, 1024);
    if (rc != header_size)
        return rc < 0 ? rc : -ECOMM;

    // Interpret the header to determine size of the bulk
    ssize_t bulk_size = tmi->tmi_bulk_size_cb(tmc, sink_buf, tmi->tmi_data);
    if (bulk_size < 0)
        return bulk_size;

    // If there's no bulk proceed to request processor, else read the bulk
    return bulk_size ?
        tcp_mgr_bulk_prepare_and_recv(tmc, bulk_size, sink_buf, header_size) :
        tcp_mgr_tmi_exec_recv_cb(tmc, sink_buf, header_size);
}

static int
tcp_mgr_bulk_complete(struct tcp_mgr_connection *tmc)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);

    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;
    NIOVA_ASSERT(tmi->tmi_recv_cb);

    int rc =
        tcp_mgr_tmi_exec_recv_cb(tmc, tmc->tmc_bulk_buf, tmc->tmc_bulk_offset);

    tcp_mgr_bulk_free(tmi, tmc->tmc_bulk_buf);

    tmc->tmc_bulk_buf = NULL;
    tmc->tmc_bulk_offset = 0;

    return rc;
}

static void
tcp_mgr_conn_recv_inline(struct tcp_mgr_connection *tmc)
{
    NIOVA_ASSERT(tmc);

    int rc = 0;

    // is this a new RPC?
    if (!tmc->tmc_bulk_remain)
    {
        NIOVA_ASSERT(!tmc->tmc_bulk_buf);

        rc = tcp_mgr_new_msg_handler(tmc);
        if (rc == -EAGAIN) // Nothing to do on this socket
            return;

        if (rc < 0)
            SIMPLE_LOG_MSG(LL_NOTIFY, "cannot read RPC, rc=%d", rc);
    }

    // no 'else', tcp_mgr_new_msg_handler can initiate bulk read
    if (rc == 0 && tmc->tmc_bulk_remain)
    {
        NIOVA_ASSERT(tmc->tmc_bulk_buf);

        rc = tcp_mgr_bulk_progress_recv(tmc);
        if (rc < 0)
            SIMPLE_LOG_MSG(LL_NOTIFY, "cannot complete bulk read, rc=%d", rc);

        else if (!tmc->tmc_bulk_remain)
            rc = tcp_mgr_bulk_complete(tmc);
    }

    if (rc < 0)
    {
        SIMPLE_LOG_MSG(LL_DEBUG, "error in recv, closing");

        // Will remove from epoll set
        tcp_mgr_connection_close_internal(tmc);
    }
}

static epoll_mgr_cb_ctx_t
tcp_mgr_conn_recv_handoff(struct tcp_mgr_connection *tmc)
{
    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;

    niova_mutex_lock(&tmi->tmi_connq.tmcq_mutex);
    if (tmc->tmc_handoff)
    {
        SIMPLE_LOG_MSG(LL_WARN, "tmc=%p handoff already set", tmc);
        goto out;
    }

    tmc->tmc_handoff = 1;

    SIMPLE_LOG_MSG(LL_DEBUG, "tmc=%p", tmc);

    NIOVA_SET_COND_AND_WAKE_LOCKED(
        signal,
        { STAILQ_INSERT_TAIL(&tmi->tmi_connq.tmcq_queue, tmc, tmc_lentry); },
        &tmi->tmi_connq.tmcq_cond);

out:
    niova_mutex_unlock(&tmi->tmi_connq.tmcq_mutex);
}

static epoll_mgr_cb_ctx_t
tcp_mgr_recv_cb(const struct epoll_handle *eph, uint32_t events)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);

    NIOVA_ASSERT(eph && eph->eph_arg);

    struct tcp_mgr_connection *tmc = eph->eph_arg;
    DBG_TCP_MGR_CXN(LL_TRACE, tmc, "received events: %d", events);

    int rc = tcp_mgr_epoll_handle_rc_get(eph, events);
    if (rc)
    {
        if (rc == -ECONNRESET || rc == -ENOTCONN || rc == -ECONNABORTED)
            tcp_mgr_connection_close_internal(tmc);

        SIMPLE_LOG_MSG(LL_NOTIFY, "error received on socket fd=%d, rc=%d",
                       eph->eph_fd, rc);
        return;
    }

    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;
    NIOVA_ASSERT(tmi);

    return tmi->tmi_conn_recv_handoff ?
        tcp_mgr_conn_recv_handoff(tmc) :
        tcp_mgr_conn_recv_inline(tmc);
}

static int
tcp_mgr_connection_merge_incoming(struct tcp_mgr_connection *incoming,
                                  struct tcp_mgr_connection *owned)
{
    NIOVA_ASSERT(incoming && owned && incoming->tmc_status == TMCS_CONNECTING);

    struct tcp_mgr_instance *tmi = incoming->tmc_tmi;

    if (owned->tmc_status != TMCS_DISCONNECTED)
    {
        if (owned->tmc_status == TMCS_NEEDS_SETUP)
            DBG_TCP_MGR_CXN(LL_ERROR, owned, "connection not setup");
        else
            DBG_TCP_MGR_CXN(LL_NOTIFY, owned,
                            "incoming connection, but outgoing conn exists,"
                            " status: %d",
                            owned->tmc_status);

        // if peer is attempting to connect again, assume old connection is dead
        // XXX it would be better to send a ping, and handle failure there
        if (owned->tmc_status == TMCS_CONNECTED)
            tcp_mgr_connection_close_internal(owned);

        tcp_mgr_connection_put(owned);

        SIMPLE_LOG_MSG(LL_ERROR, "handshake error");
        return -EINVAL;
    }

    owned->tmc_header_size = incoming->tmc_header_size;
    owned->tmc_tsh.tsh_socket = incoming->tmc_tsh.tsh_socket;
    owned->tmc_status = TMCS_CONNECTED;

    if (incoming->tmc_eph.eph_installed)
        epoll_handle_del(tmi->tmi_epoll_mgr, &incoming->tmc_eph);

    uint32_t events = EPOLLIN | (tmi->tmi_conn_recv_handoff ? EPOLLONESHOT : 0);

    tcp_mgr_connection_epoll_add(owned, events, tcp_mgr_recv_cb,
                                 tmi->tmi_connection_ref_cb);

    DBG_TCP_MGR_CXN(LL_NOTIFY, owned, "connection established");

    // epoll takes a ref, so we can give up ours
    tcp_mgr_connection_put(owned);
    return 0;
}

static epoll_mgr_cb_ctx_t
tcp_mgr_handshake_cb(const struct epoll_handle *eph, uint32_t events)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);
    NIOVA_ASSERT(eph && eph->eph_arg);

    (void)events;
    struct tcp_mgr_connection *tmc = eph->eph_arg;
    struct tcp_mgr_instance *tmi = tmc->tmc_tmi;

    struct iovec iov;
    tcp_mgr_handshake_iov_init(tmi, &iov);

    int rc = tcp_socket_recv(&tmc->tmc_tsh, &iov, 1, NULL, 1);
    if (rc <= 0)
    {
        if (rc == 0)
            SIMPLE_LOG_MSG(LL_NOTIFY, "tcp_socket_recv(): connection closed");
        else
            SIMPLE_LOG_MSG(LL_ERROR, "tcp_socket_recv(): %s", strerror(-rc));

        tcp_mgr_handshake_iov_fini(&iov);
        tcp_mgr_incoming_fini_err(tmc);

        return;
    }

    SIMPLE_LOG_MSG(LL_TRACE, "tcp_socket_recv(): rc=%d", rc);

    struct tcp_mgr_connection *new_tmc = NULL;

    rc = tmi->tmi_handshake_cb(tmi->tmi_data, &new_tmc, &tmc->tmc_header_size,
                               tmc->tmc_tsh.tsh_socket, iov.iov_base, rc);

    tcp_mgr_handshake_iov_fini(&iov);

    if (rc == 0)
        rc = tcp_mgr_connection_merge_incoming(tmc, new_tmc);

    if (rc)
        tcp_mgr_incoming_fini_err(tmc);
    else
        tcp_mgr_incoming_fini(tmc);
}

static int
tcp_mgr_accept(struct tcp_mgr_instance *tmi, int fd)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);

    struct tcp_mgr_connection *tmc = tcp_mgr_incoming_new(tmi);
    if (!tmc)
    {
        SIMPLE_LOG_MSG(LL_ERROR, "raft_net_tcp_incoming_new(): NOMEM");
        return -ENOMEM;
    }

    int rc = tcp_socket_handle_accept(fd, &tmc->tmc_tsh);
    if (rc < 0)
    {
        SIMPLE_LOG_MSG(LL_ERROR, "tcp_socket_handle_accept(): %d", rc);
        tcp_mgr_incoming_fini_err(tmc);

        return rc;
    }

    tcp_mgr_connection_epoll_add(tmc, EPOLLIN, tcp_mgr_handshake_cb, NULL);

    return 0;
}

static epoll_mgr_cb_ctx_t
tcp_mgr_listen_cb(const struct epoll_handle *eph, uint32_t events)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);
    NIOVA_ASSERT(eph && eph->eph_arg);

    (void)events;

    struct tcp_mgr_instance *tmi = eph->eph_arg;

    int rc = tcp_mgr_accept(tmi, eph->eph_fd);
    if (rc < 0)
        SIMPLE_LOG_MSG(LL_ERROR, "tcp_mgr_accept(): %d", rc);
}

/* must call sockets_setup and sockets_bind first */
int
tcp_mgr_epoll_setup(struct tcp_mgr_instance *tmi, struct epoll_mgr *epoll_mgr,
                    bool tmi_listen)
{
    if (!tmi || !epoll_mgr)
        return -EINVAL;

    tmi->tmi_epoll_mgr = epoll_mgr;

    // raft client do not need listen socket.
    if (!tmi_listen)
        return 0;

    int rc = epoll_handle_init(&tmi->tmi_listen_eph,
                               tmi->tmi_listen_socket.tsh_socket, EPOLLIN,
                               tcp_mgr_listen_cb, tmi, NULL);

    return rc ? rc : epoll_handle_add(epoll_mgr, &tmi->tmi_listen_eph);
}

static int
tcp_mgr_connection_epoll_mod(struct tcp_mgr_connection *tmc, int op,
                             epoll_mgr_cb_t cb)
{
    NIOVA_ASSERT(tmc);

    if (cb)
        tmc->tmc_eph.eph_cb = cb;

    if (op)
        tmc->tmc_eph.eph_events = op;

    return epoll_handle_mod(tmc->tmc_tmi->tmi_epoll_mgr, &tmc->tmc_eph);
};

static int
tcp_mgr_handshake_fill_send(struct tcp_mgr_connection *tmc)
{
    struct iovec handshake_iov;
    tcp_mgr_handshake_iov_init(tmc->tmc_tmi, &handshake_iov);

    size_t header_size =
        tmc->tmc_tmi->tmi_handshake_fill(tmc->tmc_tmi->tmi_data, tmc,
                                         handshake_iov.iov_base,
                                         handshake_iov.iov_len);
    tmc->tmc_header_size = header_size;

    int rc = tcp_socket_send(&tmc->tmc_tsh, &handshake_iov, 1);

    tcp_mgr_handshake_iov_fini(&handshake_iov);

    return rc;
}

static int
tcp_mgr_connect_complete(struct tcp_mgr_connection *tmc)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);

    int rc;

    if (tmc->tmc_status != TMCS_CONNECTING)
    {
        DBG_TCP_MGR_CXN(LL_ERROR, tmc, "unexpected connection status %d",
                        tmc->tmc_status);
        rc = -EAGAIN;
        goto out;
    }

    DBG_TCP_MGR_CXN(LL_DEBUG, tmc, "reinstalling epoll handler");
    rc = tcp_mgr_connection_epoll_mod(tmc, EPOLLIN, tcp_mgr_recv_cb);
    if (rc < 0)
    {
        DBG_TCP_MGR_CXN(LL_DEBUG, tmc, "error reinstalling epoll handler");
        goto out;
    }

    rc = tcp_mgr_handshake_fill_send(tmc);
    if (rc < 0)
        goto out;

    DBG_TCP_MGR_CXN(LL_NOTIFY, tmc, "connection established");
    tmc->tmc_status = TMCS_CONNECTED;
    rc = 0;
out:
    if (rc < 0)
        tcp_mgr_connection_close_internal(tmc);

    return rc;
}

static epoll_mgr_cb_ctx_t
tcp_mgr_connect_cb(const struct epoll_handle *eph, uint32_t events)
{
    NIOVA_ASSERT(eph && eph->eph_arg);

    struct tcp_mgr_connection *tmc = eph->eph_arg;
    SIMPLE_LOG_MSG(LL_NOTIFY,
                   "tcp_mgr_connect_cb(): connecting to %s:%d[%p]",
                   tmc->tmc_tsh.tsh_ipaddr, tmc->tmc_tsh.tsh_port, tmc);

    int rc = tcp_mgr_epoll_handle_rc_get(eph, events);
    if (rc < 0)
    {
        SIMPLE_LOG_MSG(LL_NOTIFY, "raft_net_tcp_connect_cb(): error, rc=%d",
                       rc);
        tcp_mgr_connection_close_internal(tmc);
        return;
    }

    tcp_mgr_connect_complete(tmc);
}

static void
tcp_mgr_connection_connect_epoll_ctx(struct tcp_mgr_connection *tmc)
{
    if (tmc->tmc_status != TMCS_DISCONNECTED)
    {
        DBG_TCP_MGR_CXN(LL_NOTIFY, tmc, "no longer disconnected");
        return;
    }

    tmc->tmc_status = TMCS_CONNECTING;

    int rc = tcp_socket_setup(&tmc->tmc_tsh);
    if (rc < 0)
    {
        SIMPLE_LOG_MSG(LL_NOTIFY, "tcp_socket_setup(): %d", rc);
        return;
    }

    rc = tcp_mgr_connection_epoll_add(tmc, EPOLLOUT, tcp_mgr_connect_cb,
                                      tmc->tmc_tmi->tmi_connection_ref_cb);
    if (rc < 0)
    {
        SIMPLE_LOG_MSG(LL_NOTIFY, "tcp_mgr_connection_epoll_add(): %d", rc);
        return;
    }

    rc = tcp_socket_connect(&tmc->tmc_tsh);
    if (rc < 0 && rc != -EINPROGRESS)
    {
        tcp_mgr_connection_close_internal(tmc);
        SIMPLE_LOG_MSG(LL_WARN, "tcp_socket_connect(): %d", rc );
    }
    else if (rc == -EINPROGRESS)
    {
        SIMPLE_LOG_MSG(LL_DEBUG, "waiting for connect callback, fd = %d",
                       tmc->tmc_tsh.tsh_socket);
    }
    else
    {
        tcp_mgr_connect_complete(tmc);
    }

    return;
}

static int
tcp_mgr_connection_verify(struct tcp_mgr_connection *tmc,
                          bool do_connect)
{
    enum tcp_mgr_connection_status status = tmc->tmc_status;

    DBG_TCP_MGR_CXN(LL_TRACE, tmc, "do_connect=%d status=%d", do_connect,
                    status);

    NIOVA_ASSERT(tmc && status != TMCS_NEEDS_SETUP);

    int rc = 0;
    if (status != TMCS_DISCONNECTED || !do_connect)
    {
        if (status == TMCS_CONNECTED)
            rc = 0;
        else if (status == TMCS_CONNECTING)
            rc = -EALREADY;
        else if (!do_connect || status == TMCS_DISCONNECTING)
            rc = -ENOTCONN;
        else
            rc = -EINVAL;

        return rc;
    }

    // can't set status to CONNECTING here because we aren't in epoll_ctx
    tcp_mgr_connection_epoll_ctx_run(tmc, tcp_mgr_connection_connect_epoll_ctx);

    return -EAGAIN;
}

int
tcp_mgr_send_msg(struct tcp_mgr_connection *tmc, struct iovec *iov,
                 size_t niovs)
{
    SIMPLE_FUNC_ENTRY(LL_TRACE);
    if (tmc->tmc_status == TMCS_NEEDS_SETUP)
    {
        DBG_TCP_MGR_CXN(LL_ERROR, tmc, "connection not setup");
        return -EINVAL;
    }

    int rc = tcp_mgr_connection_verify(tmc, true);
    DBG_TCP_MGR_CXN(LL_DEBUG, tmc, "tcp_mgr_connection_verify(): %d", rc);
    if (rc < 0)
        return rc;

    const ssize_t total_size = niova_io_iovs_total_size_get(iov, niovs);

    niova_mutex_lock(&tmc->tmc_send_mutex);

    ssize_t send_rc = tcp_socket_send(&tmc->tmc_tsh, iov, niovs);

    niova_mutex_unlock(&tmc->tmc_send_mutex);

    if (send_rc != total_size)
    {
        DBG_TCP_MGR_CXN(LL_WARN, tmc,
                        "tcp_socket_send(): returns=%zd, expected=%zd",
                        send_rc, total_size);
        tcp_mgr_connection_close(tmc);
    }

    return rc;
}

void
tcp_mgr_connection_close(struct tcp_mgr_connection *tmc)
{
    tcp_mgr_connection_epoll_ctx_run(tmc, tcp_mgr_connection_close_internal);
};
