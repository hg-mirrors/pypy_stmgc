
TESTS = $(wildcard *.duh)

DEBUG = $(addprefix debug-,$(TESTS))
BUILD = $(addprefix build-,$(TESTS))
TIMER = $(addprefix timer-,$(TESTS))

all: $(DEBUG) $(BUILD)

timer: $(TIMER)


debug-%:
	../duhton_debug $*

build-%:
	../duhton $*

timer-%:
	bash -c "time ../duhton $*" > /dev/null
	@echo
