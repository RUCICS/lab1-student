CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic

.PHONY: all clean grade run shell

all: mini-tmux

mini-tmux: mini_tmux.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f mini-tmux

# Run mini-tmux interactively inside Docker (Linux environment)
run:
	docker run --rm -it \
		-v $(PWD):/src:ro \
		--entrypoint sh mini-tmux-harness -c \
		'cp -r /src /build && cd /build && make clean >/dev/null 2>&1; make && SHELL=/bin/bash exec /build/mini-tmux'

# Drop into a Docker shell with mini-tmux built (for manual testing, attach, etc.)
shell:
	docker run --rm -it \
		-v $(PWD):/src:ro \
		--entrypoint bash mini-tmux-harness -c \
		'cp -r /src /build && cd /build && make clean >/dev/null 2>&1; make && echo "=== mini-tmux built at /build/mini-tmux ===" && cd /build && exec bash'

# Black-box grading via Docker (compiles inside container for Linux compatibility)
grade:
	docker run --rm \
		-v $(PWD):/src:ro \
		mini-tmux-harness grade-src
