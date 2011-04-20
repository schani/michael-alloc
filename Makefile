#TEST = -DTEST_QUEUE
#TEST = -DTEST_ALLOC
TEST = -DTEST_DELAYED_FREE

ALLOC_C = alloc.c

QUEUE = queue
#QUEUE = test-queue

test : hazard.c $(QUEUE).c $(ALLOC_C) mono-mmap.c sgen-gc.c delayed-free.c
	gcc -O2 $(TEST) -g -Wall -o test hazard.c $(QUEUE).c $(ALLOC_C) mono-mmap.c sgen-gc.c delayed-free.c -lpthread $(shell pkg-config --cflags --libs glib-2.0)
