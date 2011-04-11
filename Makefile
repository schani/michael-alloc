#QUEUE = queue
QUEUE = test-queue

test : hazard.c $(QUEUE).c alloc.c mono-mmap.c sgen-gc.c
	gcc -O0 -g -Wall -o test hazard.c $(QUEUE).c alloc.c mono-mmap.c sgen-gc.c -lpthread $(shell pkg-config --cflags --libs glib-2.0)
