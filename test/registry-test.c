/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2019
 */

#include "niova_backtrace.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ctor.h"
#include "ctl_interface.h"
#include "ctl_interface_cmd.h"
#include "init.h"
#include "registry.h"
#include "log.h"

REGISTRY_ENTRY_FILE_GENERATE;

LREG_ROOT_ENTRY_GENERATE(test_entry_init_ctx, LREG_USER_TYPE_UNIT_TEST0);
LREG_ROOT_ENTRY_GENERATE(test_entry, LREG_USER_TYPE_UNIT_TEST1);
LREG_ROOT_ENTRY_GENERATE(issue_799, LREG_USER_TYPE_UNIT_TEST0);

struct issue_799_chunk
{
    struct lreg_node i799_lrn;
    const char       *i799_vdev_uuid;
    uint64_t          i799_number;
    char              i799_shallow_status[LREG_VALUE_STRING_MAX + 1];
};

static int
issue_799_merge_info_cb(enum lreg_node_cb_ops op, struct lreg_node *lrn,
                        struct lreg_value *lv)
{
    struct issue_799_chunk *chunk = lrn->lrn_cb_arg;
    NIOVA_ASSERT(chunk);

    switch (op)
    {
    case LREG_NODE_CB_OP_GET_NAME:
        if (!lv)
            return -EINVAL;
        strncpy(lv->lrv_key_string, "merge-info", LREG_VALUE_STRING_MAX);
        lv->get.lrv_num_keys_out = 1;
        break;
    case LREG_NODE_CB_OP_READ_VAL:
        if (!lv)
            return -EINVAL;
        if (lv->lrv_value_idx_in)
            return -ERANGE;
        lreg_value_fill_string(lv, "shallow-status",
                               chunk->i799_shallow_status);
        break;
    case LREG_NODE_CB_OP_WRITE_VAL:
        if (!lv)
            return -EINVAL;
        if (lv->lrv_value_idx_in)
            return -ERANGE;
        snprintf(chunk->i799_shallow_status,
                 sizeof(chunk->i799_shallow_status), "%s",
                 LREG_VALUE_TO_IN_STR(lv));
        break;
    case LREG_NODE_CB_OP_INSTALL_NODE:        // fall through
    case LREG_NODE_CB_OP_INSTALL_QUEUED_NODE: // fall through
    case LREG_NODE_CB_OP_DESTROY_NODE:
        break;
    default:
        return -ENOENT;
    }

    return 0;
}

static int
issue_799_chunk_cb(enum lreg_node_cb_ops op, struct lreg_node *lrn,
                   struct lreg_value *lv)
{
    struct issue_799_chunk *chunk = lrn->lrn_cb_arg;
    NIOVA_ASSERT(chunk);

    switch (op)
    {
    case LREG_NODE_CB_OP_GET_NAME:
        if (!lv)
            return -EINVAL;
        strncpy(lv->lrv_key_string, "chunk", LREG_VALUE_STRING_MAX);
        lv->get.lrv_num_keys_out = 3;
        break;
    case LREG_NODE_CB_OP_READ_VAL:
        if (!lv)
            return -EINVAL;
        switch (lv->lrv_value_idx_in)
        {
        case 0:
            lreg_value_fill_unsigned(lv, "number", chunk->i799_number);
            break;
        case 1:
            lreg_value_fill_string(lv, "vdev-uuid", chunk->i799_vdev_uuid);
            break;
        case 2:
            lreg_value_fill_vobject(lv, "merge-info",
                                    LREG_USER_TYPE_UNIT_TEST0,
                                    issue_799_merge_info_cb);
            break;
        default:
            return -ERANGE;
        }
        break;
    case LREG_NODE_CB_OP_WRITE_VAL:
        return -EOPNOTSUPP;
    case LREG_NODE_CB_OP_INSTALL_NODE:        // fall through
    case LREG_NODE_CB_OP_INSTALL_QUEUED_NODE: // fall through
    case LREG_NODE_CB_OP_DESTROY_NODE:
        break;
    default:
        return -ENOENT;
    }

    return 0;
}

static struct issue_799_chunk issue799Chunks[] = {
    {
        .i799_vdev_uuid = "00000000-0000-0000-0000-000000000001",
        .i799_number = 7,
        .i799_shallow_status = "idle",
    },
    {
        .i799_vdev_uuid = "00000000-0000-0000-0000-000000000002",
        .i799_number = 8,
        .i799_shallow_status = "idle",
    },
};

static int
null_lrn_cb(enum lreg_node_cb_ops op, struct lreg_node *lrn,
            struct lreg_value *lv)
{
    (void)op;
    (void)lrn;
    (void)lv;

    return 0;
}

static struct lreg_node null_lrn =
{.lrn_cb = null_lrn_cb,
 .lrn_user_type = LREG_USER_TYPE_UNIT_TEST2,
 .lrn_statically_allocated = 1};

static struct lreg_node inlined_child_lrn =
{.lrn_cb = null_lrn_cb,
 .lrn_user_type = LREG_USER_TYPE_UNIT_TEST4,
 .lrn_inlined_member = 1};

static void
registry_test_wait_for_install_or_removal(struct lreg_node *lrn, bool install)
{
    int rc = lreg_node_wait_for_completion(lrn, install);
    NIOVA_ASSERT(!rc);
}

static void
registry_test_install_and_remove_node(void)
{
    struct lreg_node *lrn = LREG_ROOT_ENTRY_PTR(test_entry);

    if (lreg_node_is_installed(lrn) || !lrn->lrn_statically_allocated ||
        lrn->lrn_async_install || lrn->lrn_root_node ||
        lrn->lrn_vnode_child || lrn->lrn_inlined_member ||
        lrn->lrn_async_remove)
    {
        DBG_LREG_NODE(LL_FATAL, lrn, "invalid state detected (pre)");
    }

    int rc = lreg_node_install(lrn, lreg_root_node_get());
    if (rc || !lrn->lrn_async_install || lrn->lrn_async_remove)
        DBG_LREG_NODE(LL_FATAL, lrn,
                      "invalid state detected (status=%s)",
                      strerror(-rc));

    registry_test_wait_for_install_or_removal(lrn, true);

    rc = lreg_node_remove(lrn, lreg_root_node_get());
    if (rc || !lrn->lrn_async_install || !lrn->lrn_async_remove)
        DBG_LREG_NODE(LL_FATAL, lrn,
                      "invalid state detected (status=%s)",
                      strerror(-rc));

    registry_test_wait_for_install_or_removal(lrn, false);
}

static void
registry_test_issue_799_apply_to_vobject(void)
{
    char tmpdir[] = "/tmp/niova-registry-issue-799-XXXXXX";

    NIOVA_ASSERT(mkdtemp(tmpdir));

    int dirfd = open(tmpdir, O_RDONLY | O_DIRECTORY);
    NIOVA_ASSERT(dirfd >= 0);

    int cmdfd = openat(dirfd, "cmd", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    NIOVA_ASSERT(cmdfd >= 0);

    int rc = dprintf(
        cmdfd,
        "APPLY shallow-status@merge\n"
        "WHERE /issue_799/number@7&vdev-uuid@%s/merge-info\n"
        "OUTFILE /out\n",
        issue799Chunks[0].i799_vdev_uuid);
    NIOVA_ASSERT(rc > 0);
    NIOVA_ASSERT(!close(cmdfd));

    struct ctli_cmd_handle cch = {
        .ctlih_reg_user_type = LREG_USER_TYPE_ANY,
        .ctlih_output_dirfd = dirfd,
        .ctlih_input_dirfd = dirfd,
        .ctlih_input_file_name = "cmd",
    };

    rc = ctlic_process_request(&cch);
    NIOVA_ASSERT(!rc);

    NIOVA_ASSERT(!strncmp(issue799Chunks[0].i799_shallow_status, "merge",
                          LREG_VALUE_STRING_MAX));
    NIOVA_ASSERT(!strncmp(issue799Chunks[1].i799_shallow_status, "idle",
                          LREG_VALUE_STRING_MAX));

    NIOVA_ASSERT(!unlinkat(dirfd, "cmd", 0));
    NIOVA_ASSERT(!unlinkat(dirfd, "out", 0));
    NIOVA_ASSERT(!close(dirfd));
    NIOVA_ASSERT(!rmdir(tmpdir));
}

int
main(void)
{
    FUNC_ENTRY(LL_WARN);
    registry_test_install_and_remove_node();
    registry_test_issue_799_apply_to_vobject();

    return 0;
}

/**
 * registry_test_init - acts as a unit test in the init_ctx() mode where all
 *    registry ops may be done synchronously w/out the help of the util thread.
 *    Here the basic cases are tested.
 */
static init_ctx_t NIOVA_CONSTRUCTOR(UNIT_TEST_CTOR_PRIORITY)
registry_test_init(void)
{
    FUNC_ENTRY(LL_WARN);

    // Install via macro which wraps lreg_node_install()
    LREG_ROOT_ENTRY_INSTALL(test_entry_init_ctx);
    LREG_ROOT_ENTRY_INSTALL(issue_799);

    struct lreg_node *lrn = LREG_ROOT_ENTRY_PTR(test_entry_init_ctx);

    if (!lreg_node_is_installed(lrn) || !lrn->lrn_statically_allocated ||
        lrn->lrn_async_install || lrn->lrn_root_node ||
        lrn->lrn_vnode_child || lrn->lrn_inlined_member ||
        lrn->lrn_async_remove)
    {
        DBG_LREG_NODE(LL_FATAL, lrn, "invalid state detected");
    }

    lreg_node_init(&null_lrn, LREG_USER_TYPE_UNIT_TEST2, null_lrn_cb, NULL,
                   LREG_INIT_OPT_INLINED_CHILDREN);
    NIOVA_ASSERT(lreg_node_children_are_inlined(&null_lrn));

    /* Removing node before installation should fail w/ -EINVAL since, in this
     * case, the parent has no children attached.
     */
    int rc = lreg_node_remove(&null_lrn, lrn);
    NIOVA_ASSERT(rc == -EINVAL);

    // Install an 'inlined' child into the null_lrn
    rc = lreg_node_install(&inlined_child_lrn, &null_lrn);
    NIOVA_ASSERT(!rc);

    // Install a test node onto our root lrn
    rc = lreg_node_install(&null_lrn, lrn);
    NIOVA_ASSERT(!rc);

    // Adding node again should fail w/ -EALREADY
    rc = lreg_node_install(&null_lrn, lrn);
    NIOVA_ASSERT(rc == -EALREADY);

    /* Removing a node which has not yet been installed when the parent has
     * at least one entry.
     */
    struct lreg_node test_lrn = {.lrn_cb = null_lrn_cb,
        .lrn_user_type = LREG_USER_TYPE_UNIT_TEST3};

    rc = lreg_node_remove(&test_lrn, &null_lrn);
    NIOVA_ASSERT(rc == -EALREADY);

    // Will fail because test_lrn is not an inlined member
    rc = lreg_node_install(&test_lrn, &null_lrn);
    NIOVA_ASSERT(rc == -EINVAL);

    for (size_t i = 0; i < ARRAY_SIZE(issue799Chunks); i++)
    {
        lreg_node_init(&issue799Chunks[i].i799_lrn,
                       LREG_USER_TYPE_UNIT_TEST0, issue_799_chunk_cb,
                       &issue799Chunks[i], LREG_INIT_OPT_NONE);
        rc = lreg_node_install(&issue799Chunks[i].i799_lrn,
                               LREG_ROOT_ENTRY_PTR(issue_799));
        NIOVA_ASSERT(!rc);
    }

    // Removing 'lrn' should fail w/ -EBUSY since it has a child
    rc = lreg_node_remove(lrn, lreg_root_node_get());
    NIOVA_ASSERT(rc == -EBUSY);
}

static destroy_ctx_t NIOVA_DESTRUCTOR(UNIT_TEST_CTOR_PRIORITY)
registry_test_destroy(void)
{
    FUNC_ENTRY(LL_WARN);

    struct lreg_node *lrn = LREG_ROOT_ENTRY_PTR(test_entry_init_ctx);

    // null_lrn should still be installed
    NIOVA_ASSERT(!CIRCLEQ_EMPTY(&lrn->lrn_head));
    NIOVA_ASSERT((CIRCLEQ_FIRST(&lrn->lrn_head) ==
                  CIRCLEQ_LAST(&lrn->lrn_head)) &&
                 CIRCLEQ_FIRST(&lrn->lrn_head) == &null_lrn);

    // Removing 'lrn' should fail w/ -EBUSY since it has a child
    int rc = lreg_node_remove(lrn, lreg_root_node_get());
    NIOVA_ASSERT(rc == -EBUSY);

    NIOVA_ASSERT(lreg_node_children_are_inlined(&null_lrn));
    rc = lreg_node_remove(&null_lrn, lrn);
    NIOVA_ASSERT(!rc);

    if (!lreg_node_is_installed(lrn) || !lrn->lrn_statically_allocated ||
        lrn->lrn_async_install || lrn->lrn_root_node ||
        lrn->lrn_vnode_child || lrn->lrn_inlined_member ||
        lrn->lrn_async_remove)
    {
        DBG_LREG_NODE(LL_FATAL, lrn, "invalid state detected (pre)");
    }

    // Remove the lrn entry now w/ the macro (equiv to lreg_node_remove())
    LREG_ROOT_ENTRY_REMOVE(test_entry_init_ctx);

    for (size_t i = 0; i < ARRAY_SIZE(issue799Chunks); i++)
    {
        rc = lreg_node_remove(&issue799Chunks[i].i799_lrn,
                              LREG_ROOT_ENTRY_PTR(issue_799));
        NIOVA_ASSERT(!rc);
    }
    LREG_ROOT_ENTRY_REMOVE(issue_799);

    if (lreg_node_is_installed(lrn) || lrn->lrn_async_remove)
    {
        DBG_LREG_NODE(LL_FATAL, lrn, "invalid state detected (post)");
    }
}
