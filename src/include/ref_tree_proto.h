/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2018
 */

#ifndef REF_TREE_H
#define REF_TREE_H 1

#include <pthread.h>

#include "log.h"
#include "tree.h"

#define REF_TREE_PROTOTYPE RB_PROTOTYPE

#define REF_TREE_LOCK(head)                                             \
    if (!(head)->bypass_mutex) pthread_mutex_lock(&(head)->mutex);

#define REF_TREE_UNLOCK(head)                                           \
    if (!(head)->bypass_mutex) pthread_mutex_unlock(&(head)->mutex);

#define REF_TREE_SET_MUTEX_BYPASS(head) (head)->bypass_mutex = 1

/**
 * REF_TREE_INIT_ALT_REF - "ALT_REF" means "use an alternate initial ref cnt".
 */
#define REF_TREE_INIT_ALT_REF(rt, constructor_fn, destructor_fn, ref, \
                              user_arg)                               \
    {                                                                 \
        pthread_mutex_init(&(rt)->mutex, NULL);                       \
        (rt)->initial_ref_cnt = ref;                                  \
        RB_INIT(&(rt)->rt_head);                                      \
        (rt)->constructor = constructor_fn;                           \
        (rt)->destructor = destructor_fn;                             \
        (rt)->arg = user_arg;                                         \
    }

#define REF_TREE_INITIAL_REF_CNT(rt) (rt)->initial_ref_cnt

#define REF_TREE_INIT(rt, constructor_fn, destructor_fn, arg)           \
    REF_TREE_INIT_ALT_REF(rt, constructor_fn, destructor_fn, 1, arg);

#define REF_TREE_DESTROY(rt)                 \
    {                                        \
        pthread_mutex_destroy(&(rt)->mutex); \
    }

#define REF_TREE_HEAD(name, type)                                      \
    RB_HEAD(_RT_##name, type);                                         \
    struct name                                                        \
    {                                                                  \
        struct _RT_##name rt_head;                                     \
        unsigned int    initial_ref_cnt:31;                            \
        unsigned int    bypass_mutex:1;                                \
        pthread_mutex_t mutex;                                         \
        void           *arg;                                           \
        struct type  *(*constructor)(const struct type *, void *);     \
        int           (*destructor)(struct type *, void *);            \
    }

#define REF_TREE_REF_INCREASE_ELEM_LOCKED(elm, field, cnt)      \
    do {                                                        \
        (elm)->field.rte_ref_cnt += cnt;                        \
        NIOVA_ASSERT((elm)->field.rte_ref_cnt > 0);             \
    } while (0)

#define REF_TREE_REF_DECREASE_ELEM_LOCKED(elm, field, cnt)      \
    do {                                                        \
        (elm)->field.rte_ref_cnt -= cnt;                        \
        NIOVA_ASSERT((elm)->field.rte_ref_cnt > 0);             \
    } while (0)

#define REF_TREE_REF_GET_ELEM_LOCKED(elm, field)    \
    do {                                            \
        NIOVA_ASSERT((elm)->field.rte_ref_cnt > 0); \
        (elm)->field.rte_ref_cnt++;                 \
    } while (0)

// Take a ref on an already held element
#define REF_TREE_REF_GET_ELEM(head, elm, field)     \
    do {                                            \
        REF_TREE_LOCK(head);                        \
        REF_TREE_REF_GET_ELEM_LOCKED(elm, field);   \
        REF_TREE_UNLOCK(head);       \
    } while (0)

#define REF_TREE_REF_PUT_ELEM_LOCKED(elm, field)    \
    do {                                            \
        (elm)->field.rte_ref_cnt--;                 \
        NIOVA_ASSERT((elm)->field.rte_ref_cnt > 0); \
    } while (0)

#define REF_TREE_REF_GET_ELEM_SERIALIZED REF_TREE_REF_GET_ELEM_LOCKED
#define REF_TREE_REF_PUT_ELEM_SERIALIZED REF_TREE_REF_PUT_ELEM_LOCKED

// Must not release final reference
#define REF_TREE_REF_PUT_ELEM(head, elm, field)          \
    do {                                                \
        REF_TREE_LOCK(head);                            \
        REF_TREE_REF_PUT_ELEM_LOCKED(elm, field);       \
        REF_TREE_UNLOCK(head);                          \
    } while (0)

#define REF_TREE_MIN(name, head, type, field)         \
    ({                                                \
        struct type *elm = NULL;                      \
        REF_TREE_LOCK(head);                          \
        elm = RB_MIN(_RT_##name, &(head)->rt_head);   \
        if (elm)                                      \
            REF_TREE_REF_GET_ELEM_LOCKED(elm, field); \
        REF_TREE_UNLOCK(head);                        \
        elm;                                          \
    })

#define REF_TREE_ENTRY(type)             \
struct {                                 \
    RB_ENTRY_PACKED(type) RTE_RBE;       \
    int rte_ref_cnt;                     \
}

#define REF_TREE_GENERATE(name, type, field, cmp)                    \
    RB_GENERATE(_RT_##name, type, field.RTE_RBE, cmp);               \
                                                                     \
    bool                                                             \
    name##_PUT(struct name *head, struct type *elm)                  \
    {                                                                \
        bool removed = false;                                        \
        REF_TREE_LOCK(head);                                         \
        elm->field.rte_ref_cnt--;                                    \
        NIOVA_ASSERT(elm->field.rte_ref_cnt >= 0);                   \
        if (!elm->field.rte_ref_cnt)                                 \
        {                                                            \
            struct type *old =                                       \
                RB_REMOVE(_RT_##name, &head->rt_head, elm);          \
            removed = true;                                          \
            NIOVA_ASSERT(elm == old);                                \
        }                                                            \
        REF_TREE_UNLOCK(head);                          \
        if (removed)                                                 \
            head->destructor(elm, head->arg);                        \
        return removed;                                              \
    }                                                                \
                                                                     \
    static struct type *                                             \
    name##_LOOKUP_LOCKED(struct name *head,                          \
                         const struct type *lookup_elm)              \
    {                                                                \
        struct type *elm = RB_FIND(_RT_##name, &head->rt_head,       \
                                   (struct type *)lookup_elm);       \
        if (elm)                                                     \
            REF_TREE_REF_GET_ELEM_LOCKED(elm, field);                \
                                                                     \
        return elm;                                                  \
    }                                                                \
                                                                     \
    struct type *                                                    \
    name##_GET(struct name *head, const struct type *lookup_elm,     \
               const bool add, int *ret)                             \
    {                                                                \
        if (ret)                                                     \
            *ret = 0;                                                \
                                                                     \
        REF_TREE_LOCK(head);                            \
        struct type *elm = name##_LOOKUP_LOCKED(head, lookup_elm);   \
        REF_TREE_UNLOCK(head);                          \
                                                                     \
        if (elm || !add)                                             \
        {                                                            \
            if (add && ret)                                          \
                *ret = -EEXIST;                                      \
                                                                     \
            return elm;                                              \
        }                                                            \
                                                                     \
        elm = head->constructor(lookup_elm, head->arg);              \
        if (!elm)                                                    \
        {                                                            \
            if (ret)                                                 \
                *ret = -ENOMEM;                                      \
                                                                     \
            return NULL;                                             \
        }                                                            \
                                                                     \
        REF_TREE_LOCK(head);                                         \
                                                                     \
        struct type *already = RB_INSERT(_RT_##name, &head->rt_head, \
                                         elm);                       \
        if (already)                                                 \
            REF_TREE_REF_GET_ELEM_LOCKED(already, field);            \
        else                                                         \
            elm->field.rte_ref_cnt = head->initial_ref_cnt;          \
                                                                     \
        REF_TREE_UNLOCK(head);                                       \
                                                                     \
        if (already)                                                 \
        {                                                            \
            (int)head->destructor(elm, head->arg);                   \
            elm = already;                                           \
            if (ret)                                                 \
                *ret = -EALREADY;                                    \
        }                                                            \
                                                                     \
        return elm;                                                  \
    }                                                                \

#define RT_GET(name, head, lookup_elm, add, ret) \
    name##_GET(head, lookup_elm, add, ret)

#define RT_LOOKUP(name, head, lookup_elm) \
    name##_GET(head, lookup_elm, false, NULL)

#define RT_GET_ADD(name, head, lookup_elm, ret) \
    name##_GET(head, lookup_elm, true, ret)

#define RT_PUT(name, head, elm)                 \
    name##_PUT(head, elm)

#define RT_FOREACH_LOCKED(x, name, head) \
    RB_FOREACH(x, _RT_##name, &(head)->rt_head)


#define RT_FOREACH_SAFE_LOCKED(x, name, head, y) \
    RB_FOREACH_SAFE(x, _RT_##name, &(head)->rt_head, y)

#define RT_FOREACH_REVERSE_LOCKED(x, name, head) \
    RB_FOREACH_REVERSE(x, _RT_##name, &(head)->rt_head)

#define RT_FOREACH_REVERSE_SAFE_LOCKED(x, name, head, y) \
    RB_FOREACH_REVERSE_SAFE(x, _RT_##name, &(head)->rt_head, y)

#define RT_MIN_SERIALIZED_NOREF(name, head)     \
    RB_MIN(_RT_##name, &(head)->rt_head)

#define RT_NEXT_SERIALIZED_NOREF(name, x)       \
    RB_NEXT(_RT_##name, x)

#define RT_REMOVE_IGNORE_REF_LOCKED(name, head, elm)    \
    RB_REMOVE(_RT_##name, &(head)->rt_head, elm);

#define RT_FOREACH_SERIALIZED              RT_FOREACH_LOCKED
#define RT_FOREACH_SAFE_SERIALIZED         RT_FOREACH_SAFE_LOCKED
#define RT_FOREACH_REVERSE_SERIALIZED      RT_FOREACH_REVERSE_LOCKED
#define RT_FOREACH_REVERSE_SAFE_SERIALIZED RT_FOREACH_REVERSE_SAFE_LOCKED
#define RT_REMOVE_IGNORE_REF_SERIALIZED    RT_REMOVE_IGNORE_REF_LOCKED

#define RT_EMPTY(head) RB_EMPTY(&(head)->rt_head)
#define RT_INIT(head) RB_INIT(&(head)->rt_head)

// To traverse a tree with macro such as RT_FOREACH, RT_FOREACH_SAFE from a
// source code file different from the one with the definition of the tree (by
// REF_TREE_GENERATE), you have to declare the methods necessary for the
// compiler to find the correct names.
#define DECLARE_FOR_RT_TRAVERSE(name, type)                                   \
struct type *                                                                  \
_RT_##name##_RB_MINMAX(struct _RT_##name *head, int val);                      \
struct type *                                                                  \
_RT_##name##_RB_NEXT(struct type *elm);                                        \

#endif //REF_TREE_H
