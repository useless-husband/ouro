# ouro: a self-hosting C compiler for arm64 macOS.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
SRCS    := $(wildcard src/*.c)
HDRS    := src/ouro.h

.PHONY: all test regress bootstrap clean lint asan

all: build/ouro

build/ouro: $(SRCS) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS)

test: build/ouro
	scripts/regress.sh build/ouro

regress: build/ouro
	scripts/regress.sh build/ouro

# stage 1 = ouro built by the system compiler
# stage 2 = ouro built by stage 1
# stage 3 = ouro built by stage 2; must be byte-identical to stage 2
bootstrap: build/ouro
	scripts/bootstrap.sh

lint:
	@mkdir -p build
	$(CC) -std=c11 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter \
	  -Wno-missing-field-initializers -Werror $(SRCS)

asan:
	@mkdir -p build/asan
	$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(WARN) \
	  -o build/asan/ouro $(SRCS)
	scripts/regress.sh build/asan/ouro

clean:
	rm -rf build
