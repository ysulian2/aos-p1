CC	= gcc
CPPFLAGS= -MMD -MP
CFLAGS	= -pedantic -Wall -std=gnu99 -O2
DEBUGFLAGS = -g -O0 -DDEBUG

AR	= ar
ARFLAGS	= r
RM	= rm -rf

BUILDDIR = build
SRCS = $(wildcard *.c) $(wildcard rb_tree/*.c)
OBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
DEPS = $(patsubst %.c,$(BUILDDIR)/%.d,$(SRCS))

TGT = libgtthreads.a

all: $(TGT)

$(TGT): $(OBJS)
	$(AR) $(ARFLAGS) $@ $(OBJS) $(LIBS)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(COMPILE.c) -o $@ $<

-include $(DEPS)

debug: clean
	@$(MAKE) CFLAGS="$(CFLAGS) $(DEBUGFLAGS)"

clean:
	@$(RM) $(TGT) $(BUILDDIR)
