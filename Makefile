CC = gcc
CFLAGS = -Wall -Wextra -O2 -I/opt/homebrew/opt/openssl@3/include
LDFLAGS = -L/opt/homebrew/opt/openssl@3/lib -lcrypto

SRCS = object.c tree.c index.c commit.c pes.c
OBJS = $(SRCS:.c=.o)

pes: $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c pes.h
	$(CC) $(CFLAGS) -c $< -o $@

test_objects: test_objects.o object.o
	$(CC) -o $@ $^ $(LDFLAGS)

test_tree: test_tree.o object.o tree.o index.o
	$(CC) -o $@ $^ $(LDFLAGS)

.PHONY: all clean test test-unit test-integration

all: pes test_objects test_tree

clean:
	rm -f pes test_objects test_tree pes.o object.o tree.o index.o commit.o test_objects.o test_tree.o
	rm -rf .pes

test: test-unit test-integration

test-unit: test_objects test_tree
	./test_objects
	./test_tree

test-integration: pes
	bash test_sequence.sh
