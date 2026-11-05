/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */

#define _GNU_SOURCE 1
#include <pthread.h>
#undef _GNU_SOURCE
#include <signal.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "thread.h"

REGISTRY_ENTRY_FILE_GENERATE;

__thread char thrName[MAX_THREAD_NAME + 1];
__thread const struct thread_ctl *thrCtl;
__thread const char *thrCoName; // Coroutine name
__thread int threadCoID;        // Coroutine ID
__thread int threadCoYieldCnt;  // Coroutine yield counter

static void
thr_ctl_basic_sighandler(int signum)
{
    (void)signum;
    struct thread_ctl *tc = (struct thread_ctl *)thrCtl;

    if (tc)
        tc->tc_sig_cnt++;
}

static void
thread_ctl_install_sighandlers(void)
{
    static bool handlers_installed = false;
    if (handlers_installed)
        return;

    handlers_installed = true;

    struct sigaction oldact;
    FATAL_IF_strerror((sigaction(SIGALRM, NULL, &oldact)), "sigaction: ");

    struct sigaction newact = oldact;
    newact.sa_handler = &thr_ctl_basic_sighandler;
    newact.sa_flags = 0;
    sigemptyset(&newact.sa_mask);

    FATAL_IF_strerror((sigaction(SIGALRM, &newact, NULL)), "sigaction: ");
}

static thread_exec_ctx_t
thread_ctl_monitor_via_watchdog_internal(struct thread_ctl *tc)
{
    NIOVA_ASSERT(!thread_ctl_has_flag(tc, TC_FLAG_WATCHDOG));

    int rc = watchdog_add_thread(&tc->tc_watchdog_handle);

    if (!rc)
    {
        rc = thread_ctl_set_flag(tc, TC_FLAG_WATCHDOG);
    }

    DBG_THREAD_CTL((rc ? LL_ERROR : LL_DEBUG), tc, "%s",
                   rc ? strerror(-rc) : "");
}

/**
 * thread_ctl_monitor_via_watchdog - optional public interface for users who
 *    wish to add a thread to the watchdog at some point after calling
 *    thread_create().  However, the preferred method for watchdog usage is
 *    by calling thread_create_watched().
 *
 */
thread_exec_ctx_t
thread_ctl_monitor_via_watchdog(struct thread_ctl *tc)
{
    thread_ctl_monitor_via_watchdog_internal(tc);
}

thread_exec_ctx_bool_t
thread_ctl_should_continue(const struct thread_ctl *tc)
{
    return thread_ctl_has_flag(tc, TC_FLAG_HALT) ? false : true;
}

thread_exec_ctx_bool_t
thread_ctl_should_continue_self(void)
{
    return thrCtl ? thread_ctl_should_continue(thrCtl) : true;
}

static thread_exec_ctx_t
thread_ctl_inc_iteration_cnt(struct thread_ctl *tc)
{
    if (tc)
        watchdog_inc_exec_cnt(&tc->tc_watchdog_handle);
}

thread_exec_ctx_bool_t
thread_ctl_should_pause(struct thread_ctl *tc)
{
    /* Bump the iteration counter here so that continual pauses don't
     * trigger the watchdog.
     */
    thread_ctl_inc_iteration_cnt(tc);

    if (!thread_ctl_should_continue(tc))
    {
        /* Thread is halting, no need to wait.
         */
        return false;
    }
    else if (!thread_ctl_has_flag(tc, TC_FLAG_RUN))
    {
        /* Thread control asked this thread to stop running for now.
         */
        tc->tc_pause_usecs = THR_PAUSE_DEFAULT_USECS;

        return true;
    }

    return false;
}

thread_exec_ctx_t
thread_ctl_pause(struct thread_ctl *tc)
{
    FATAL_IF_strerror((usleep(tc->tc_pause_usecs) && errno != EINTR),
                      "usleep() failure");

    tc->tc_pause_usecs = 0;
}

thread_exec_ctx_t
thread_ctl_pause_if_should(struct thread_ctl *tc)
{
    while (thread_ctl_should_pause(tc))
        thread_ctl_pause(tc);
}

thread_exec_ctx_bool_t
thread_ctl_loop_test(struct thread_ctl *tc)
{
    bool should_continue = thread_ctl_should_continue(tc);

    if (should_continue)
    {
        thread_ctl_pause_if_should(tc);
        should_continue = thread_ctl_should_continue(tc);
    }

    return should_continue;
}

void
thread_ctl_run(struct thread_ctl *tc)
{
    int rc = thread_ctl_set_flag(tc, TC_FLAG_RUN);
    FATAL_IF(rc != 0, "TC_FLAG_RUN could not be set, %s", strerror(-rc));

    DBG_THREAD_CTL(LL_NOTIFY, tc, "");
}

void
thread_ctl_halt(struct thread_ctl *tc)
{
    int rc = thread_ctl_set_flag(tc, TC_FLAG_HALT);
    FATAL_IF(rc != 0, "TC_FLAG_HALT could not be set, %s", strerror(-rc));

    DBG_THREAD_CTL(LL_NOTIFY, tc, "");
}

thread_id_t
thread_id_get(void)
{
    return pthread_self();
}

const char *
thread_name_get(void)
{
    pthread_getname_np(pthread_self(), thrName, MAX_THREAD_NAME);
    return (const char *)thrName;
}

static int
thread_create_internal(void *(*start_routine)(void *), struct thread_ctl *tc,
                       const char *name, void *arg, const pthread_attr_t *attr,
                       bool use_watchdog)
{
    memset(tc, 0, sizeof(struct thread_ctl));

    tc->tc_arg = arg;

    /* The new thread inherits this mask. A termination signal sent to the
     * process then goes to an application thread that waits for it.
     */
    sigset_t termination_signals;
    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGINT);
    sigaddset(&termination_signals, SIGTERM);
    sigaddset(&termination_signals, SIGHUP);
    sigaddset(&termination_signals, SIGQUIT);

    sigset_t creator_mask;
    pthread_sigmask(SIG_BLOCK, &termination_signals, &creator_mask);

    int rc = pthread_create(&tc->tc_thread_id, attr, start_routine, tc);

    pthread_sigmask(SIG_SETMASK, &creator_mask, NULL);

    if (!rc && name && strnlen(name, MAX_THREAD_NAME) < MAX_THREAD_NAME)
    {
        strncpy(tc->tc_thr_name, name, MAX_THREAD_NAME);
        rc = pthread_setname_np(tc->tc_thread_id, name);

        if (!rc && use_watchdog)
            thread_ctl_monitor_via_watchdog_internal(tc);
    }

    return rc;
}

int
thread_create(void *(*start_routine)(void *), struct thread_ctl *tc,
              const char *name, void *arg, const pthread_attr_t *attr)
{
    thread_ctl_install_sighandlers();

    return thread_create_internal(start_routine, tc, name, arg, attr, false);
}

int
thread_create_watched(void *(*start_routine)(void *), struct thread_ctl *tc,
                      const char *name, void *arg, const pthread_attr_t *attr)
{
    return thread_create_internal(start_routine, tc, name, arg, attr, true);
}

void
thread_creator_wait_until_ctl_loop_reached(const struct thread_ctl *tc)
{
    if (tc)
        while (!thread_ctl_thread_has_reached_ctl_loop(tc))
            usleep(100);
}

void
thread_ctl_remove_from_watchdog(struct thread_ctl *tc)
{
    if (tc && thread_ctl_has_flag(tc, TC_FLAG_WATCHDOG))
    {
        int rc = watchdog_remove_thread(&tc->tc_watchdog_handle);
        if (rc)
        {
            NIOVA_ASSERT(rc == -ENOENT);
        }
        else
        {
            rc = thread_ctl_unset_flag(tc, TC_FLAG_WATCHDOG);
            NIOVA_ASSERT(rc == 0);
        }
    }
}

static long int
thread_join_internal(struct thread_ctl *tc, bool blocking)
{
    if (!tc)
        return -EINVAL;

    void *retval;
    int rc = blocking ?
        pthread_join(tc->tc_thread_id, &retval) :
        pthread_tryjoin_np(tc->tc_thread_id, &retval);

    if (rc)
        return -rc;

    else if ((long int *)retval)
        return *(long int *)retval;

    return 0;
}

long int
thread_join(struct thread_ctl *tc)
{
    return thread_join_internal(tc, true);
}

long int
thread_join_nb(struct thread_ctl *tc)
{
    return thread_join_internal(tc, false);
}

int
thread_issue_sig_alarm_to_thread(pthread_t tid)
{
    return tid > 0 ? pthread_kill(tid, SIGALRM) : -EINVAL;
}

int
thread_halt_and_destroy(struct thread_ctl *tc)
{
    if (!tc->tc_thread_id)
        return -EINVAL;

    thread_ctl_halt(tc);

    thread_ctl_remove_from_watchdog(tc);

    if (tc->tc_waiting_cond && tc->tc_waiting_mutex)
    {
        NIOVA_SET_COND_AND_WAKE(broadcast, {}, tc->tc_waiting_mutex,
                                tc->tc_waiting_cond);
    }

    void *retval;
    int my_errno = 0;

    int kill_rc = thread_issue_sig_alarm_to_thread(tc->tc_thread_id);
    if (kill_rc == ESRCH) // If the thread is already gone, ignore the error.
        kill_rc = 0;

    int rc = pthread_join(tc->tc_thread_id, &retval);

    if (rc)
        my_errno = errno;

    const enum log_level log_level =
        (kill_rc || rc || (long int *)retval) ? LL_WARN : LL_NOTIFY;

    DBG_THREAD_CTL(log_level, tc,
                   "pthread_join(): rc=%d:%d errno=%s, thr_retval=%p",
                   kill_rc, rc, strerror(my_errno), (long int *)retval);

    if (!rc)
        tc->tc_thread_id = 0;

    return rc;
}

void
thread_abort(void)
{
    abort();
}
