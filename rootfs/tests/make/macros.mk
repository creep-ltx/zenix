# macros: ordinary, expanded when used, := at once, +=, a suffix and a pattern replacement, nesting
NAME = world
GREETING = hello $(NAME)
SRC = a.c b.c c.c
OBJ = $(SRC:.c=.o)
PAT = $(SRC:%.c=obj/%.o)
NAME2 := $(NAME)
NAME += again
WHICH = SRC
all:
	@echo "$(GREETING)"
	@echo "$(OBJ)"
	@echo "$(PAT)"
	@echo "[$(NAME2)] [$(NAME)]"
	@echo "[$(UNDEF)] [$(CMD)] [$(FROMENV)]"
	@echo "$($(WHICH))"
