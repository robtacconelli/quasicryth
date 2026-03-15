CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=199309L
LDFLAGS = -lz -lbz2 -lm

SRCS = ht.c md5.c ac.c fib.c tok.c cb.c compress.c decompress.c main.c
OBJS = $(SRCS:.c=.o)
LIB_OBJS = ht.o md5.o ac.o fib.o tok.o cb.o compress.o decompress.o
TARGET = qtc

all: $(TARGET) ab_test

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

ab_test: ab_test.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Dependencies
ht.o:         ht.c ht.h
md5.o:        md5.c md5.h
ac.o:         ac.c ac.h ht.h
fib.o:        fib.c fib.h qtc.h
tok.o:        tok.c tok.h ac.h ht.h
cb.o:         cb.c cb.h qtc.h ht.h
compress.o:   compress.c qtc.h ht.h ac.h fib.h tok.h cb.h md5.h
decompress.o: decompress.c qtc.h ht.h ac.h fib.h tok.h cb.h md5.h
main.o:       main.c qtc.h
ab_test.o:    ab_test.c qtc.h ht.h ac.h fib.h tok.h cb.h

clean:
	rm -f $(OBJS) ab_test.o $(TARGET) ab_test

.PHONY: all clean
