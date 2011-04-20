#TEST_QUEUE = -DTEST_QUEUE
TEST_ALLOC = -DTEST_ALLOC

ALLOC_C = alloc.c

QUEUE = queue
#QUEUE = test-queue

test : hazard.c $(QUEUE).c $(ALLOC_C) mono-mmap.c sgen-gc.c delayed-free.c
	gcc -O0 $(TEST_QUEUE) $(TEST_ALLOC) -g -Wall -o test hazard.c $(QUEUE).c $(ALLOC_C) mono-mmap.c sgen-gc.c delayed-free.c -lpthread $(shell pkg-config --cflags --libs glib-2.0)
