#ifndef PSRAM_STACK_TEST_BK_LIST_H
#define PSRAM_STACK_TEST_BK_LIST_H

#include <stddef.h>

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

#define INIT_LIST_HEAD(head) \
	do { \
		(head)->next = (head); \
		(head)->prev = (head); \
	} while (0)

static inline void list_add(struct list_head *node, struct list_head *head)
{
	node->next = head->next;
	node->prev = head;
	head->next->prev = node;
	head->next = node;
}

static inline void list_add_tail(
	struct list_head *node,
	struct list_head *head)
{
	node->next = head;
	node->prev = head->prev;
	head->prev->next = node;
	head->prev = node;
}

static inline void list_del(struct list_head *node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
}

#define list_entry(pointer, type, member) \
	((type *)((char *)(pointer) - offsetof(type, member)))

#define list_for_each_entry(position, head, member) \
	for ((position) = list_entry((head)->next, __typeof__(*(position)), member); \
		&(position)->member != (head); \
		(position) = list_entry( \
			(position)->member.next, __typeof__(*(position)), member))

#endif
