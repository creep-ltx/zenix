# suffix rules, chained (hello.txt -> hello.up -> hello.len), the internal macros $@ $< $*
.SUFFIXES: .txt .up .len
.txt.up:
	@echo "up $< -> $@ ($*)"
	cat $< $< > $@
	@sh wait2.sh
.up.len:
	@echo "len $< -> $@ ($*)"
	cat $< $< > $@
	@sh wait2.sh
