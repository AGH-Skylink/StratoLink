.PHONY: all

all: stratolink ground

stratolink: stratolink.c e32.h
	gcc stratolink.c -o stratolink -lgpiod -Wall -Wextra -Werror -O3

ground: ground.c e32.h
	gcc ground.c -o ground -lgpiod -Wall -Wextra -Werror -O3
