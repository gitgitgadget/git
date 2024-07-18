# Run this via `FILTER_EXTENSIONS=contrib/filter-extensions/rand/rand.a make`
# from the main git directory. That way we inherit useful variables.

ifneq ($(findstring s,$(MAKEFLAGS)),s)
ifndef V
	QUIET_CC       = @echo '   ' CC $@;
	QUIET_AR       = @echo '   ' AR $@;
endif
endif

FILTER_STATIC_LIB = rand.a

FILTER_CFLAGS = "-I../../.."

all: $(FILTER_STATIC_LIB)
ifeq ($(MAKELEVEL),0)
	$(error "Run via parent git make")
endif
	@:

$(FILTER_STATIC_LIB): rand.o
	$(QUIET_AR)$(AR) $(ARFLAGS) $@ $^

rand.o: rand.c
	$(QUIET_CC)$(CC) -c $(ALL_CFLAGS) $(FILTER_CFLAGS) $<

clean:
	$(RM) $(FILTER_STATIC_LIB) rand.o

.PHONY: all clean
