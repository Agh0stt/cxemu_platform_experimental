CC     = gcc
CFLAGS = -O2 -std=c11 -Wall -Wextra -Iinclude
TARGET = cxemu
TOOLCHAIN_DIR ?= ../../cxis_toolchain_new/cxis_new
CXAS   = $(TOOLCHAIN_DIR)/cxas
CXLD   = $(TOOLCHAIN_DIR)/cxld

all: $(TARGET) bios/abios.cxe

$(TARGET): cxemu.c include/abios.h include/cxis.h include/cxe.h
	$(CC) $(CFLAGS) -o $@ cxemu.c

bios/abios.cxe: abios.cxis
	$(CXAS) abios.cxis -o bios/abios.cxo
	$(CXLD) bios/abios.cxo -b 0x00000500 -e abios_reset -o bios/abios.cxe

disk.img:
	dd if=/dev/zero of=disk.img bs=512 count=2048

clean:
	rm -f $(TARGET) bios/abios.cxo bios/abios.cxe *.o

.PHONY: all clean
