TARGET = kxo
kxo-objs = main.o game.o xoroshiro.o mcts.o negamax.o zobrist.o
obj-m := $(TARGET).o

ccflags-y := -std=gnu99 -Wno-declaration-after-statement
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

GIT_HOOKS := .git/hooks/applied
all: kmod xo-user

kmod: $(GIT_HOOKS) main.c
	$(MAKE) -C $(KDIR) M=$(PWD) modules

xo-user: xo-user.c coro.c \
         user_space_ai/mcts.c user_space_ai/negamax.c \
         user_space_ai/zobrist.c user_space_ai/xoroshiro.c game.c 
	$(CC) $(ccflags-y) -Iuser_space_ai -o $@ $^

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo


clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) xo-user
