#TEST = -DTEST_DELAYED_FREE
#TEST = -DTEST_QUEUE
TEST = -DTEST_ALLOC

ALLOC_C = lock-free-alloc.c

QUEUE = lock-free-queue
#QUEUE = test-queue

test : hazard.c $(QUEUE).c $(ALLOC_C) mono-mmap.c sgen-gc.c delayed-free.c test.c
	gcc -O2 $(TEST) -g -Wall -o test hazard.c $(QUEUE).c $(ALLOC_C) mono-mmap.c sgen-gc.c delayed-free.c test.c -lpthread $(shell pkg-config --cflags --libs glib-2.0)
