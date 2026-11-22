export PS4_PAYLOAD_SDK ?= /home/rob/Work/Coding/ps4tor/ps4-payload-sdk
include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk

ELF := tor-ps4.elf
SRC := main.c

CFLAGS  := -O2 -Wall
LDFLAGS := -ltor -levent -lssl -lcrypto -lz

all: $(ELF)

$(ELF): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(ELF)
