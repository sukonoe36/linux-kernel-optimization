# biolatency loader links the userspace program against libbpf. CI compiles
# only the BPF object (see `check`) to avoid requiring libbpf on the runner.

CLANG   ?= clang
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
BPFTOOL ?= bpftool
ARCH    := $(shell uname -m | sed 's/x86_64/x86/;s/aarch64/arm64/;s/ppc64le/powerpc/;s/mips.*/mips/;s/riscv64/riscv/;s/loongarch64/loongarch/')

BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) -I. -Wall

.PHONY: all build clean check help

all: build

help:
	@echo "Targets:"
	@echo "  build  - compile biolatency.bpf.c and the userspace loader"
	@echo "  check  - compile the BPF object only (used by CI)"
	@echo "  clean  - remove generated objects and binaries"

vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format header > $@

biolatency.bpf.o: biolatency.bpf.c vmlinux.h
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

biolatency: biolatency.c biolatency.bpf.o
	$(CC) $(CFLAGS) -o $@ $< -lbpf -lelf -lz

build: biolatency.bpf.o biolatency

check: biolatency.bpf.o

clean:
	rm -f biolatency.bpf.o biolatency vmlinux.h
