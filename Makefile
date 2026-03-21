CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
LDFLAGS = -lz -lbz2 -llzma -lm

SRCS = ht.c md5.c ac.c fib.c tok.c cb.c compress.c decompress.c main.c
OBJS = $(SRCS:.c=.o)
LIB_OBJS = ht.o md5.o ac.o fib.o tok.o cb.o compress.o decompress.o
TARGET = qtc

all: $(TARGET)

$(TARGET): $(OBJS)
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
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
