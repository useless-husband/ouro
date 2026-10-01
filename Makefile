# ouro: a self-hosting C compiler for arm64 macOS.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
SRCS    := $(wildcard src/*.c)
HDRS    := src/ouro.h

.PHONY: all test regress abi bootstrap c-testsuite lua zlib fuzz clean lint sanitize

all: build/ouro

build/ouro: $(SRCS) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS)

test: build/ouro
	scripts/check.sh build/ouro
	scripts/regress.sh build/ouro
	scripts/abi.sh build/ouro 1 2 3
	scripts/fuzz.sh build/ouro 1 40

abi: build/ouro
	scripts/abi.sh build/ouro 1 2 3 4 5 6 7 8 9 10

regress: build/ouro
	scripts/regress.sh build/ouro

# stage 1 = ouro built by the system compiler
# stage 2 = ouro built by stage 1
# stage 3 = ouro built by stage 2; must be byte-identical to stage 2
bootstrap: build/ouro
	scripts/bootstrap.sh

c-testsuite: build/ouro
	scripts/c-testsuite.sh build/ouro -O0
	scripts/c-testsuite.sh build/ouro -O1

lua: build/ouro
	scripts/lua.sh build/ouro -O1

zlib: build/ouro
	scripts/zlib.sh build/ouro -O1

# FUZZ_SEEDS=N make fuzz runs more random programs
fuzz: build/ouro
	scripts/fuzz.sh build/ouro 1 $${FUZZ_SEEDS:-500}

lint:
	@mkdir -p build
	$(CC) -std=c11 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter \
	  -Wno-missing-field-initializers -Werror $(SRCS)

# ouro built with sanitizers, then used to run the regression and ABI
# tests.  SANITIZE=address,undefined adds ASan where it works (its
# runtime hangs at startup on the macOS 27 beta used for development).
SANITIZE ?= undefined
sanitize:
	@mkdir -p build/sanitize
	$(CC) -std=c11 -O1 -g -fsanitize=$(SANITIZE) -fno-sanitize-recover=all \
	  -fno-omit-frame-pointer $(WARN) -o build/sanitize/ouro $(SRCS)
	scripts/regress.sh build/sanitize/ouro
	scripts/abi.sh build/sanitize/ouro 1

clean:
	rm -rf build
