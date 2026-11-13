# .PHONY, include, .DEFAULT, a double-colon rule, a macro from the command line in a recursive make
include inc.mk
-include nothere.mk
.PHONY: clean sub
clean:
	@echo clean ran $(INC)
sub:
	@$(MAKE) -f inc2.mk SUBMSG=from-top
dc::
	@echo first of dc
dc::
	@echo second of dc
.DEFAULT:
	@echo default for $@
