#ifndef CORO_H
#define CORO_H

#include <setjmp.h>
#include "./user_space_ai/user_list.h"

struct task {
    jmp_buf env;
    struct list_head list;
    char task_name[10];
    int n;
    int i;
};

struct arg {
    int n;
    int i;
    char *task_name;
};

extern struct list_head tasklist;
extern void (**tasks)(void *);
extern struct arg *args;
extern int ntasks;
extern jmp_buf sched;
extern struct task *cur_task;

// 由 coro.c 提供的函式與變數
void schedule(void);
void task_add(struct task *task);
void task_switch(void);
extern struct task *cur_task;

#endif
