#TEST = -DTEST_DELAYED_FREE
#TEST = -DTEST_QUEUE
TEST = -DTEST_ALLOC

ALLOC = lock-free-alloc

QUEUE = lock-free-queue
#QUEUE = test-queue

all : test

%.o : %.c
	gcc -O2 -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

delayed-free.o : delayed-free.c
	gcc -O2 -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

hazard.o : hazard.c
	gcc -O2 -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

lock-free-queue.o : lock-free-queue.c
	gcc -O2 -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

test.o : test.c
	gcc -O2 -g $(TEST) -c -Wall $(shell pkg-config --cflags glib-2.0) $<

test : hazard.o $(QUEUE).o $(ALLOC).o mono-mmap.o sgen-gc.o delayed-free.o test.o
	gcc -O2 -g -Wall -o test hazard.o $(QUEUE).o $(ALLOC).o mono-mmap.o sgen-gc.o delayed-free.o test.o -lpthread $(shell pkg-config --libs glib-2.0)
