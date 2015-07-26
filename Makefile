all:
	@echo Note: The SCGI C Library is designed to not require system-wide
	@echo installation, instead, you can simply put scgilib.c and scgilib.h
	@echo in your project directly.  This makefile is only for making the
	@echo helloworld.c test program.
	@echo
	gcc -Wall -Wextra -pedantic -g scgilib.c helloworld.c -o helloworld
