#TEST = -DTEST_DELAYED_FREE
#TEST = -DTEST_QUEUE
#TEST = -DTEST_ALLOC
TEST = -DTEST_LLS

ALLOC = lock-free-alloc

QUEUE = lock-free-queue
#QUEUE = test-queue

OPT = -O0

CFLAGS = $(TEST) $(OPT) -g -Wall $(shell pkg-config --cflags glib-2.0) -DFAILSAFE_DELAYED_FREE

all : test

%.o : %.c
	gcc $(CFLAGS) -c  $<

delayed-free.o : delayed-free.c
	gcc $(CFLAGS) -c  $<

hazard.o : hazard.c
	gcc $(CFLAGS) -c  $<

lock-free-queue.o : lock-free-queue.c
	gcc $(CFLAGS) -c  $<

mono-linked-list-set.o : mono-linked-list-set.c
	gcc $(CFLAGS) -c  $<

test.o : test.c
	gcc $(CFLAGS) -c  $<

test : hazard.o $(QUEUE).o $(ALLOC).o mono-mmap.o sgen-gc.o delayed-free.o mono-linked-list-set.o test.o
	gcc $(OPT) -g -Wall -o test hazard.o $(QUEUE).o $(ALLOC).o mono-mmap.o sgen-gc.o delayed-free.o mono-linked-list-set.o test.o -lpthread $(shell pkg-config --libs glib-2.0)
