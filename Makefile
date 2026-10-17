CC = gcc

# Optional run-time arguments, e.g. make run ARGS="--model account --difficulty 3"
ARGS ?=
CFLAGS = -Wall -Wextra -std=c11 -D_POSIX_C_SOURCE=200809L
SRC_DIR = src
TARGET = library_tracker

SRCS = $(wildcard $(SRC_DIR)/*.c)
HDRS = $(wildcard $(SRC_DIR)/*.h)

# Homebrew OpenSSL on macOS is keg-only, so point at it explicitly if present.
# The usual install locations are checked directly (calling `brew` can stall
# while it refreshes its package index). Override with
# `make OPENSSL_PREFIX=/path/to/openssl` if yours lives elsewhere; on Linux
# none of these exist and the system OpenSSL is used.
OPENSSL_PREFIX ?= $(firstword $(wildcard \
    /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 \
    /opt/homebrew/opt/openssl /usr/local/opt/openssl))

ifneq ($(OPENSSL_PREFIX),)
    CFLAGS += -I$(OPENSSL_PREFIX)/include
    LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif

LDLIBS = -lssl -lcrypto

# On Apple Silicon Macs where only an Intel (x86_64) Homebrew OpenSSL is
# installed, build an x86_64 binary (runs fine under Rosetta 2). Override
# with `make ARCH_FLAG=` if you have a native arm64 OpenSSL instead.
UNAME_ARCH := $(shell uname -m)
ifneq ($(OPENSSL_PREFIX),)
    OPENSSL_ARCH := $(shell file -L $(OPENSSL_PREFIX)/lib/libcrypto.dylib 2>/dev/null | grep -oE 'x86_64|arm64' | head -1)
endif
ifeq ($(UNAME_ARCH),arm64)
    ifeq ($(OPENSSL_ARCH),x86_64)
        ARCH_FLAG = -arch x86_64
    endif
endif
CFLAGS += $(ARCH_FLAG)
LDFLAGS += $(ARCH_FLAG)

all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET) $(ARGS)

# Shortcuts for the two transaction models.
run-utxo: $(TARGET)
	./$(TARGET) --model utxo $(ARGS)

run-account: $(TARGET)
	./$(TARGET) --model account $(ARGS)

# Automated end-to-end tests (run in a temporary folder; your data is untouched).
test: $(TARGET)
	bash tests/run_tests.sh

# Submission archive: source, docs, tests, registries (no binary, data or keys).
DIST = Summative_Golden_Munyankindi.zip
dist:
	rm -f $(DIST)
	zip -r $(DIST) Makefile README.md books.txt members.txt copies.txt .gitignore src tests docs \
	    -x '*.DS_Store' 'docs/demo_video_script.md'

# Saved state written by the program (see README, "Disk persistence").
STATE_FILES = chain.dat pending.dat ledger.dat library_state.dat balances.dat utxo.dat meta.dat

# Removes the binary and all saved chain state (keeps user accounts and keys).
clean:
	rm -f $(TARGET) $(STATE_FILES) $(addsuffix .tmp,$(STATE_FILES))

# Full reset: also deletes all user accounts and their key pairs.
distclean: clean
	rm -f users.dat
	rm -rf keys

.PHONY: all run run-utxo run-account test dist clean distclean
