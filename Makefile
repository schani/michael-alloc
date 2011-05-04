#TEST = -DTEST_DELAYED_FREE
#TEST = -DTEST_QUEUE
#TEST = -DTEST_ALLOC
TEST = -DTEST_LLS

ALLOC = lock-free-alloc

QUEUE = lock-free-queue
#QUEUE = test-queue

OPT = -O0

all : test

%.o : %.c
	gcc $(OPT) -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

delayed-free.o : delayed-free.c
	gcc $(OPT) -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

hazard.o : hazard.c
	gcc $(OPT) -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

lock-free-queue.o : lock-free-queue.c
	gcc $(OPT) -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

mono-linked-list-set.o : mono-linked-list-set.c
	gcc $(OPT) -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

test.o : test.c
	gcc $(OPT) -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

test : hazard.o $(QUEUE).o $(ALLOC).o mono-mmap.o sgen-gc.o delayed-free.o mono-linked-list-set.o test.o
	gcc $(OPT) -g -Wall -o test hazard.o $(QUEUE).o $(ALLOC).o mono-mmap.o sgen-gc.o delayed-free.o mono-linked-list-set.o test.o -lpthread $(shell pkg-config --libs glib-2.0)
