#
# Makefile for uatool.ttp
#

TARGET = uatool.ttp
LIBS = libcmini.a
CC = m68k-atari-mint-gcc
LD = $(CC)
CPPFLAGS = 
CFLAGS = $(CPPFLAGS) -m68000 -mshort -O2 -Wall -Wundef -Wold-style-definition -fomit-frame-pointer -I../include

.PHONY: default all clean

default: $(TARGET)
all: default

OBJS = uatool.o
HEADERS = 

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(LD) startup.o $(OBJS) $(LIBS) -nostartfiles -s -o ../$@

clean:
	-rm -f ../$(TARGET) $(OBJS)
