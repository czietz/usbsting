#
# Makefile for usb_asix.stx
#

TARGET = usb_asix.stx
LIBS = 
CC = m68k-atari-mint-gcc
LD = $(CC) -mshort
CPPFLAGS = 
CFLAGS = $(CPPFLAGS) -m68000 -mshort -fno-builtin -O2 -Wall -Wundef -Wold-style-definition -fomit-frame-pointer -I../include

.PHONY: default all clean

default: $(TARGET)
all: default

OBJS = init.o usbsting.o arpcache.o asix.o utility.o
HEADERS = 

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(LD) $(OBJS) -nostartfiles -s -o ../$@

clean:
	-rm -f ../$(TARGET) $(OBJS)
