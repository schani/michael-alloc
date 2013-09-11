#TEST = -DTEST_DELAYED_FREE
#TEST = -DTEST_QUEUE
#TEST = -DTEST_ALLOC
TEST = -DTEST_LLS

ALLOC = lock-free-alloc

QUEUE = lock-free-queue
#QUEUE = test-queue

OPT = -O0

CFLAGS = $(TEST) $(OPT) -g -Wall -DMONO_INTERNAL= #-DFAILSAFE_DELAYED_FREE

all : test

%.o : %.c
	gcc $(CFLAGS) -c  $<

delayed-free.o : delayed-free.c
	gcc $(CFLAGS) -c  $<

hazard-pointer.o : hazard-pointer.c
	gcc $(CFLAGS) -c  $<

lock-free-queue.o : lock-free-queue.c
	gcc $(CFLAGS) -c  $<

mono-linked-list-set.o : mono-linked-list-set.c
	gcc $(CFLAGS) -c  $<

test.o : test.c
	gcc $(CFLAGS) -c  $<

test : hazard-pointer.o $(QUEUE).o $(ALLOC).o mono-mmap.o sgen-gc.o delayed-free.o mono-linked-list-set.o test.o
	gcc $(OPT) -g -Wall -o test hazard-pointer.o $(QUEUE).o $(ALLOC).o mono-mmap.o sgen-gc.o delayed-free.o mono-linked-list-set.o test.o -lpthread

clean :
	rm -f *.o test
