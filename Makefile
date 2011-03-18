test : hazard.c
	gcc -O0 -g -Wall -o test hazard.c $(shell pkg-config --cflags --libs glib-2.0)
