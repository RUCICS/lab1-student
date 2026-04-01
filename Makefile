CC  ?= cc
CXX ?= c++
CFLAGS   ?= -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -pedantic
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

.PHONY: all clean

all: mini-tmux

# Prefer C++ source; fall back to C source.
ifneq (,$(wildcard mini_tmux.cpp))
mini-tmux: mini_tmux.cpp
	$(CXX) $(CXXFLAGS) $< -o $@
else ifneq (,$(wildcard mini_tmux.c))
mini-tmux: mini_tmux.c
	$(CC) $(CFLAGS) $< -o $@
else
mini-tmux:
	@echo "Error: mini_tmux.cpp or mini_tmux.c not found"; exit 1
endif

clean:
	rm -f mini-tmux
