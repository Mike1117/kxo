#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "./user_space_ai/mcts.h"
#include "./user_space_ai/negamax.h"
#include "coro.h"
#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"

static char draw_buffer[DRAWBUFFER_SIZE];

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~IXON;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr, end_attr;

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 6);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 6);
            if (!read_attr)
                printf("\n\nStopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 6);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 6);
            printf("\n\nStopping the kernel space tic-tac-toe game...\n");
            break;
        }
    }
    close(attr_fd);
}

static int draw_board(char *table)
{
    int i = 0, k = 0;
    draw_buffer[i++] = '\n';
    draw_buffer[i++] = '\n';

    while (i < DRAWBUFFER_SIZE) {
        for (int j = 0; j < (BOARD_SIZE << 1) - 1 && k < N_GRIDS; j++) {
            draw_buffer[i++] = j & 1 ? '|' : table[k++];
        }
        draw_buffer[i++] = '\n';
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            draw_buffer[i++] = '-';
        }
        draw_buffer[i++] = '\n';
    }

    if (i < DRAWBUFFER_SIZE)
        draw_buffer[i] = '\0';
    else
        draw_buffer[DRAWBUFFER_SIZE - 1] = '\0';

    return 0;
}

static void run_kernel_mode(void)
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char table_buf[DRAWBUFFER_SIZE];

    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;

    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (read_attr && FD_ISSET(device_fd, &readset)) {
            FD_CLR(device_fd, &readset);
            printf("\033[H\033[J"); /* ASCII escape code to clear the screen */
            read(device_fd, table_buf, N_GRIDS);
            draw_board(table_buf);
            printf("%s", draw_buffer);
        }
    }

    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);
}

static char turn;
static int finish;
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
static char table[N_GRIDS];

static void check_win_work_func(void *arg)
{
    struct task *new_task = malloc(sizeof(struct task));
    if (!new_task) {
        fprintf(stderr, "[xo-user] new_task: memory allocation failed\n");
        return;
    }
    INIT_LIST_HEAD(&new_task->list);
    if (setjmp(new_task->env) == 0) {
        task_add(new_task);
        longjmp(sched, 1);
    }

    struct task *task = cur_task;

    if (!task) {
        fprintf(stderr, "[xo-user] task: memory allocation failed\n");
        free(new_task);
        return;
    }
    for (;;) {
        if (check_win(table) != ' ') {
            draw_board(table);
            printf("\033[H\033[J");
            printf("%s", draw_buffer);
            memset(table, ' ', N_GRIDS);
        }

        if (setjmp(task->env) == 0) {
            task_add(task);
            task_switch();
        }

        task = cur_task;
    }
}

static void drawboard_work_func(void *arg)
{
    struct task *new_task = malloc(sizeof(struct task));
    if (!new_task) {
        fprintf(stderr, "[xo-user] new_task: memory allocation failed\n");
        return;
    }
    INIT_LIST_HEAD(&new_task->list);
    if (setjmp(new_task->env) == 0) {
        task_add(new_task);
        longjmp(sched, 1);
    }

    struct task *task = cur_task;
    if (!task) {
        fprintf(stderr, "[xo-user] task: memory allocation failed\n");
        free(new_task);
        return;
    }

    for (;;) {
        if (finish) {
            draw_board(table);
            printf("\033[H\033[J");
            printf("%s", draw_buffer);
            finish = 0;
        }

        if (setjmp(task->env) == 0) {
            task_add(task);
            task_switch();
        }

        task = cur_task;
    }
}

static void ai_one_work_func(void *arg)
{
    struct task *new_task = malloc(sizeof(struct task));
    if (!new_task) {
        fprintf(stderr, "[xo-user] new_task: memory allocation failed\n");
        return;
    }
    INIT_LIST_HEAD(&new_task->list);
    if (setjmp(new_task->env) == 0) {
        task_add(new_task);
        longjmp(sched, 1);
    }

    struct task *task = cur_task;
    if (!task) {
        fprintf(stderr, "[xo-user] task: memory allocation failed\n");
        free(new_task);
        return;
    }

    for (;;) {
        if (turn == 'O') {
            int move = mcts(table, 'O');
            if (move != -1)
                table[move] = 'O';

            turn = 'X';
        }

        finish = 1;
        if (setjmp(task->env) == 0) {
            task_add(task);
            task_switch();
        }

        task = cur_task;
    }
}

static void ai_two_work_func(void *arg)
{
    struct task *new_task = malloc(sizeof(struct task));
    if (!new_task) {
        fprintf(stderr, "[xo-user] new_task: memory allocation failed\n");
        return;
    }
    INIT_LIST_HEAD(&new_task->list);
    if (setjmp(new_task->env) == 0) {
        task_add(new_task);
        longjmp(sched, 1);
    }

    struct task *task = cur_task;
    if (!task) {
        fprintf(stderr, "[xo-user] task: memory allocation failed\n");
        free(new_task);
        return;
    }

    for (;;) {
        if (turn == 'X') {
            int move;
            move = negamax_predict(table, 'X').move;

            if (move != -1)
                table[move] = 'X';

            turn = 'O';
        }
        finish = 1;

        if (setjmp(task->env) == 0) {
            task_add(task);
            task_switch();
        }

        task = cur_task;
    }
}

static void co_listen_keyboard_handler(void *arg)
{
    struct task *new_task = malloc(sizeof(struct task));
    if (!new_task) {
        fprintf(stderr, "[xo-user] new_task: memory allocation failed\n");
        return;
    }
    INIT_LIST_HEAD(&new_task->list);
    if (setjmp(new_task->env) == 0) {
        task_add(new_task);
        longjmp(sched, 1);
    }

    struct task *task = cur_task;
    if (!task) {
        fprintf(stderr, "[xo-user] task: memory allocation failed\n");
        free(new_task);
        return;
    }

    static int paused = 0;
    for (;;) {
        char input;
        ssize_t nread = read(STDIN_FILENO, &input, 1);

        if (nread == 1) {
            int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
            if (attr_fd < 0) {
                perror(
                    "co_listen_keyboard_handler: open XO_DEVICE_ATTR_FILE "
                    "failed");
            } else {
                char buf[20];
                switch (input) {
                case 16:
                    paused ^= 1;
                    if (paused) {
                        printf(
                            "\n\n[Paused] Press Ctrl-P again to resume...\n");
                        while (1) {
                            ssize_t b = read(STDIN_FILENO, &input, 1);
                            if (b == 1 && input == 16) {
                                paused = 0;
                                printf("[Resumed]\n");
                                break;
                            }
                        }
                    }
                    break;
                case 17: /* Ctrl-Q */
                    if (read(attr_fd, buf, 6) == 6) {
                        buf[4] = '1';
                        read_attr = false;
                        end_attr = true;
                        write(attr_fd, buf, 6);
                        printf(
                            "\n\nStopping the kernel space tic-tac-toe game "
                            "(or user space if modified)...\n");
                        exit(0);
                    }
                    break;
                }
                close(attr_fd);
            }
        } else if (nread == -1) {
        }

        if (setjmp(task->env) == 0) {
            task_add(task);
            task_switch();
        }
        task = cur_task;
    }
}


static void run_user_mode(void)
{
    negamax_init();
    mcts_init();
    memset(table, ' ', N_GRIDS);
    turn = 'O';
    finish = 1;

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    void (*registered_task[])(void *) = {
        ai_one_work_func,           check_win_work_func,
        co_listen_keyboard_handler, drawboard_work_func,
        ai_two_work_func,           check_win_work_func,
        co_listen_keyboard_handler, drawboard_work_func};
    tasks = registered_task;
    args = NULL;
    ntasks = ARRAY_SIZE(registered_task);

    schedule();

    raw_mode_disable();
}

int main(int argc, char *argv[])
{
    enum Mode { MODE_KERNEL, MODE_USER };
    enum Mode mode = MODE_KERNEL;

    printf("Select AI mode:\n");
    printf("1. Kernel AI (current default)\n");
    printf("2. User-space AI (coroutine)\n");
    printf("Enter choice (1/2): ");
    fflush(stdout);

    int choice = getchar();
    if (choice == '2') {
        mode = MODE_USER;
    }

    if (mode == MODE_KERNEL) {
        run_kernel_mode();
    } else {
        run_user_mode();
    }

    return 0;
}
