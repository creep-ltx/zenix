# $@ $< $? $^ $+ with prerequisites that are newer, older and repeated
out: in1 in2 in1
	@echo "@=$@ <=$< ?=$? ^=$^ +=$+"
	cat in1 in2 > out
