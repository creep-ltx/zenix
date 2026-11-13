# a failing command, -k, -i, -n, -q, -s, a failing command with a minus
all: ok1 bad1 ok2 bad2
ok1:
	@echo ok1
bad1:
	@echo bad1
	@exit 3
ok2:
	@echo ok2
bad2: bad1
	@echo bad2 ran
ign:
	-@exit 4
	@echo after the ignored one
loud:
	echo shown
	@echo quiet
