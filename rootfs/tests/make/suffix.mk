# suffix rules, chained (hello.txt -> hello.up -> hello.len), the internal macros $@ $< $*
.SUFFIXES: .txt .up .len
.txt.up:
	@echo "up $< -> $@ ($*)"
	cat $< $< > $@
	@sleep 2
.up.len:
	@echo "len $< -> $@ ($*)"
	cat $< $< > $@
	@sleep 2
