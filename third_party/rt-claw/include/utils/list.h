/*
 * Copyright (c) 2026, Chao Liu <chao.liu.zevorn@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Intrusive doubly-linked list (Linux kernel style).
 */

#ifndef CLAW_UTILS_LIST_H
#define CLAW_UTILS_LIST_H

#include <stddef.h>

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

typedef struct claw_list_node {
    struct claw_list_node *prev;
    struct claw_list_node *next;
} claw_list_node_t;

/* Static initializer: node points to itself (empty list head) */
#define CLAW_LIST_INIT(name) { &(name), &(name) }

/* Declare and initialize a list head */
#define CLAW_LIST_HEAD(name) \
    claw_list_node_t name = CLAW_LIST_INIT(name)

static inline void claw_list_init(claw_list_node_t *node)
{
    node->prev = node;
    node->next = node;
}

static inline int claw_list_empty(const claw_list_node_t *head)
{
    return head->next == head;
}

/* Insert new_node between prev and next */
static inline void claw_list__insert(claw_list_node_t *new_node,
                                     claw_list_node_t *prev,
                                     claw_list_node_t *next)
{
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

/* Add to front (after head) */
static inline void claw_list_add(claw_list_node_t *new_node,
                                 claw_list_node_t *head)
{
    claw_list__insert(new_node, head, head->next);
}

/* Add to back (before head) */
static inline void claw_list_add_tail(claw_list_node_t *new_node,
                                      claw_list_node_t *head)
{
    claw_list__insert(new_node, head->prev, head);
}

/* Remove node from its list */
static inline void claw_list_del(claw_list_node_t *node)
{
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node;
    node->next = node;
}

/* Get the struct that contains this list node */
#define claw_list_entry(ptr, type, member) \
    container_of(ptr, type, member)

/* Get the first entry, or NULL if list is empty */
#define claw_list_first_entry(head, type, member) \
    (claw_list_empty(head) ? NULL : \
     claw_list_entry((head)->next, type, member))

/* Iterate over list entries */
#define claw_list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

/* Safe iteration (allows removal of current node) */
#define claw_list_for_each_safe(pos, tmp, head) \
    for ((pos) = (head)->next, (tmp) = (pos)->next; \
         (pos) != (head); \
         (pos) = (tmp), (tmp) = (pos)->next)

/* Iterate over list entries with container_of */
#define claw_list_for_each_entry(pos, head, type, member) \
    for ((pos) = claw_list_entry((head)->next, type, member); \
         &(pos)->member != (head); \
         (pos) = claw_list_entry((pos)->member.next, type, member))

/* Iterate over list entries in reverse (tail to head) */
#define claw_list_for_each_reverse(pos, head) \
    for ((pos) = (head)->prev; (pos) != (head); (pos) = (pos)->prev)

/* Count the number of nodes */
static inline int claw_list_count(const claw_list_node_t *head)
{
    int count = 0;
    const claw_list_node_t *pos;

    for (pos = head->next; pos != head; pos = pos->next) {
        count++;
    }
    return count;
}

#endif /* CLAW_UTILS_LIST_H */
