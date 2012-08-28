CC	= gcc
CPPFLAGS= -MMD -MP
CFLAGS	= -pedantic -Wall -std=gnu99 -O2
DEBUGFLAGS = -g -O0 -DDEBUG
LDFLAGS	=
LDLIBS	= -lm

GTTHREAD_DIR = ../gtthreads
CPPFLAGS+= -I$(GTTHREAD_DIR)
LDFLAGS	+= -L$(GTTHREAD_DIR)
LDLIBS	+= -lgtthreads
GTTHREADS= $(GTTHREAD_DIR)/libgtthreads.a

RM	= rm -rf

BUILDDIR = build
SRCS = $(wildcard *.c)
OBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))
DEPS = $(patsubst %.c,$(BUILDDIR)/%.d,$(SRCS))

TGT = matrix

all: $(BUILDDIR) $(TGT)

$(BUILDDIR):
	@mkdir -p $@

$(TGT): $(OBJS) $(GTTHREADS)
	$(LINK.o) -o $@ $(OBJS) $(LDLIBS)

$(BUILDDIR)/%.o: %.c
	$(COMPILE.c) -o $@ $<

-include $(DEPS)

debug: clean
	@$(MAKE) CFLAGS="$(CFLAGS) $(DEBUGFLAGS)"

clean:
	@$(RM) $(TGT) $(BUILDDIR)
