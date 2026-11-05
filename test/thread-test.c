/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 */
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "common.h"
#include "log.h"
#include "thread.h"

static const int terminationSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};

static void *
thread_test_read_sigmask(void *arg)
{
    struct thread_ctl *tc = arg;
    sigset_t *thread_mask = thread_ctl_get_arg(tc);

    int rc = pthread_sigmask(SIG_BLOCK, NULL, thread_mask);
    FATAL_IF(rc, "pthread_sigmask(): %s", strerror(rc));

    return NULL;
}

/* A niova-core thread that catches a SIGTERM sent to the process leaves
 * nothing pending for an application thread waiting in sigsuspend().
 */
static void
thread_create_blocks_termination_signals(void)
{
    sigset_t creator_before;
    int rc = pthread_sigmask(SIG_BLOCK, NULL, &creator_before);
    FATAL_IF(rc, "pthread_sigmask(): %s", strerror(rc));

    sigset_t thread_mask;
    struct thread_ctl tc;

    rc = thread_create(thread_test_read_sigmask, &tc, "sigmask-test",
                       &thread_mask, NULL);
    FATAL_IF(rc, "thread_create(): %s", strerror(rc));

    long int join_rc = thread_join(&tc);
    FATAL_IF(join_rc, "thread_join(): %s", strerror(-join_rc));

    sigset_t creator_after;
    rc = pthread_sigmask(SIG_BLOCK, NULL, &creator_after);
    FATAL_IF(rc, "pthread_sigmask(): %s", strerror(rc));

    for (size_t sig_idx = 0; sig_idx < ARRAY_SIZE(terminationSignals);
         sig_idx++)
    {
        int signum = terminationSignals[sig_idx];

        int thread_blocks = sigismember(&thread_mask, signum);
        FATAL_IF(thread_blocks != 1,
                 "thread from thread_create() leaves %s unblocked",
                 strsignal(signum));

        int blocked_before = sigismember(&creator_before, signum);
        int blocked_after = sigismember(&creator_after, signum);
        FATAL_IF(blocked_before != blocked_after,
                 "thread_create() changed the creator's mask for %s",
                 strsignal(signum));
    }
}

int
main(void)
{
    thread_create_blocks_termination_signals();

    return 0;
}
