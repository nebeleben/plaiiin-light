TMPDIR ?= /tmp

.PHONY: test
test:
	cc -std=c11 -Wall -Werror -I../../main ../../main/ai_access.c test_ai_access.c -o $(TMPDIR)/test_ai_access && $(TMPDIR)/test_ai_access
