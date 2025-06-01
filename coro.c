/* Implementing coroutines with setjmp/longjmp */

#include "coro.h"
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LIST_HEAD(tasklist);
void (**tasks)(void *);
struct arg *args;
int ntasks;
jmp_buf sched;
struct task *cur_task;

void task_add(struct task *task)
{
    list_add_tail(&task->list, &tasklist);
}

void task_switch()
{
    if (!list_empty(&tasklist)) {
        struct task *t = list_first_entry(&tasklist, struct task, list);
        list_del(&t->list);
        cur_task = t;
        longjmp(t->env, 1);
    }
}

void schedule(void)
{
    static int i;

    setjmp(sched);

    while (ntasks-- > 0) {
        struct arg arg = args[i];
        tasks[i++](&arg);
        printf("Never reached\n");
    }

    task_switch();
}