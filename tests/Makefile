THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
root := $(realpath $(dir $(THIS_MAKEFILE))/..)
# config.mk puts the libc++fileno and libsrk31c++ flags in CXXFLAGS and LDFLAGS/LDLIBS
include $(root)/config.mk

cases := $(filter-out makefile oprofile_data,$(shell cd $(root)/tests && find -mindepth 1 -maxdepth 1 -type d -name '[a-z]*' -print | while read f; do basename $$f ; done ))
# EXCLUDE_CASES lets a caller (e.g. CI) skip specific cases. The -static cases
# visible-named/grandchildren embed DWARF 5 debug info from the system static
# libraries they pull in, which the legacy libdwarf backend cannot parse and
# which cannot practically be rebuilt as DWARF 4; the libdw backend handles them.
cases := $(filter-out $(EXCLUDE_CASES),$(cases))
$(info cases is $(cases))

# Pin the DWARF version the test cases are compiled with: each case runs its
# freshly-built binary against itself, so this controls the debug format under
# test. Default to 4 (the legacy libdwarf backend cannot parse DWARF 5, which
# modern GCC emits by default); the libdw backend can be exercised against 5 by
# passing TEST_DWARF_VERSION=5. Kept here (not just in CI) for determinism.
TEST_DWARF_VERSION ?= 4
CXXFLAGS += -I$(root)/include -O0 -fPIC -g -gdwarf-$(TEST_DWARF_VERSION) -std=c++14
LDFLAGS += -L$(root)/lib -Wl,-rpath,$(root)/lib
LDLIBS += -ldwarfpp -lelf $(LIBSRK31CXX_LIBS) $(LIBCXXFILENO_LIBS) -lboost_system -lboost_regex -lboost_filesystem -lz

.PHONY: default
default: all

# cancel builtin rule to avoid attempts at .o->executable via CC
%: %.o
# we want to use depfiles to rebuild tests, so let's write out the rule
%: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) $< -o $@ -MMD -MF .$*.d $(LDLIBS)
$(info depfiles we're maybe-including: $(patsubst %,.%.d,$(cases)))
-include $(patsubst %,.%.d,$(cases))

# these should be statically linked because they test multi-CU features
grandchildren: LDFLAGS += -pthread -static
visible-named: LDFLAGS += -pthread -static

# declare the dep, to ensure we don't test a stale binary
grandchildren: $(root)/lib/libdwarfpp.a
visible-named: $(root)/lib/libdwarfpp.a

# HACK: manually link these with libdwarf for now, because unlike the .so,
# the .a does not include it. And we must append -lelf and -lz... sigh.
# -lzstd is needed because a modern static libelf.a depends on zstd for its
# compressed-section support (the old ubuntu:18.04 libelf.a did not).
grandchildren: LDLIBS += $(LIBDWARF_LIBS) -lelf -lz -lzstd
visible-named: LDLIBS += $(LIBDWARF_LIBS) -lelf -lz -lzstd

# test case build recipes call back to us using $(MAKE) -f ../Makefile
# ... here we allow each case to define an include.mk for extra rules
-include include.mk

.PHONY: all
all: $(foreach c,$(cases),run-$(c))

# quick hack for a status report
.PHONY: report
report:
	$(MAKE) -k 2>&1 | grep '^make:.*Error'

.PHONY: clean
clean:
	for case in $(cases); do \
            $(MAKE) clean-$$case; \
        done

.PHONY: clean-%
clean-%: 
	rm -f $*/$*

.PHONY: build-%
build-%:
	$(MAKE) -C "$*" -f ../Makefile "$*"

.PHONY: run-%
run-%: # run the test case with itself as input
	$(MAKE) -C "$*" -f ../Makefile "$*" && ( cd "$*" && ./$* ./$* )

.PHONY: profile-%
profile-%:
	$(MAKE) -C "$*" "$*" && \
        sudo opcontrol --deinit && \
        sudo sh -c "echo '0' > /proc/sys/kernel/nmi_watchdog" && \
        rm -rf oprofile_data && \
        sudo opcontrol --reset && \
        sudo opcontrol --start --no-vmlinux --separate=lib,kernel --callgraph=3 && \
        ( cd "$*" && ./$* ./$* ); \
        sudo opcontrol --deinit && \
        opreport -l --callgraph $(root)/src/libdwarfpp.so $*/$* && \
        opreport -l $(root)/src/libdwarfpp.so $*/$* | head -n15

.PHONY: gdbrun-%
gdbrun-%: # run the test case with itself as input
	$(MAKE) -C "$*" -f ../Makefile "$*" && ( cd "$*" && gdb --args ./$* ./$* )

.PHONY: cleanrun-%
cleanrun-%:
	rm -f $*/$* && $(MAKE) run-$*

.PHONY: gdbcleanrun-%
gdbcleanrun-%:
	rm -f $*/$* && $(MAKE) gdbrun-$*
