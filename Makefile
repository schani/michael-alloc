test : hazard.c queue.c alloc.c mono-mmap.c sgen-gc.c
	gcc -O0 -g -Wall -o test hazard.c queue.c alloc.c mono-mmap.c sgen-gc.c -lpthread $(shell pkg-config --cflags --libs glib-2.0)
