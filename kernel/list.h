#ifndef LIST_H
#define LIST_H

struct list_node {
    struct list_node *prev;
    struct list_node *next;
};

static inline void list_init(struct list_node *list) {
    list->next = list;
    list->prev = list;
}

static inline void list_add_between(struct list_node *prev, struct list_node *next, struct list_node *node) {
    prev->next = node;
    node->prev = prev;
    node->next = next;
    next->prev = node;
}

static inline void list_add(struct list_node *list, struct list_node *node) {
    list_add_between(list, list->next, node);
}

static inline void list_add_tail(struct list_node *list, struct list_node *node) {
    list_add_between(list->prev, list, node);
}

static inline void list_del(struct list_node *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->next = node;
    node->prev = node;
}

static inline int list_empty(struct list_node *list) {
    return list->next == list;
}

// 获取结构体指针的宏 (类似 Linux 的 container_of)
#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - (uint64)(&((type *)0)->member)))

#endif