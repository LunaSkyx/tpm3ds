# Host icount build (see instrument.py). From the project root:
#   python -I tools/icount/instrument.py
#   C:\devkitPro\msys2\usr\bin\make.exe -f tools/icount/Makefile -j12   (TMP must be writable)
# Reuses the non-generated objects of build-host (build tpm_host.exe first).
CC      := gcc
GENSRC  := $(wildcard build-icount/gensrc/*.c)
GENO    := $(patsubst build-icount/gensrc/%.c,build-icount/obj/%.o,$(GENSRC))
HOSTO   := $(filter-out build-host/source/gen/%,$(shell find build-host/source -name '*.o'))

build-icount/tpm_icount.exe: $(GENO) build-icount/obj/icount_rt.o
	$(CC) -o $@ $(HOSTO) $^ -lm

build-icount/obj/%.o: build-icount/gensrc/%.c tools/icount/icount.h
	@mkdir -p build-icount/obj
	$(CC) -std=gnu11 -O1 -g0 -w -Isource -Isource/gen -Itools/icount -DCPU_FN_TRACE -DTPM_OVERRIDE_REF -c -o $@ $<

build-icount/obj/icount_rt.o: tools/icount/icount.c
	@mkdir -p build-icount/obj
	$(CC) -std=gnu11 -O2 -Isource -c -o $@ $<
