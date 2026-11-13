# a small program (the recipes wait two seconds of the wall clock: a target made in the same second as its prerequisite is out of date): dependencies, the order, what a newer file rebuilds
all: prog
prog: a.o b.o
	@echo link prog
	cat a.o b.o > prog
a.o: a.c common.h
	@echo cc a
	cp a.c a.o
	@sh wait2.sh
b.o: b.c common.h
	@echo cc b
	cp b.c b.o
	@sh wait2.sh
