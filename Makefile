test : hazard.c queue.c
	gcc -O0 -g -Wall -o test hazard.c queue.c -lpthread $(shell pkg-config --cflags --libs glib-2.0)
