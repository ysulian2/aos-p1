TGTS	= gtthreads/libgtthreads.a gtmatrix/matrix
SUBDIRS	= $(dir $(TGTS))
EXES	= $(notdir $(TGTS))

all:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d; done
	@for t in $(TGTS); do cp $$t .; done
debug:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@; done
	@for t in $(TGTS); do cp $$t .; done
clean:
	@for d in $(SUBDIRS); do $(MAKE) -C $$d $@; done
	@for e in $(EXES); do $(RM) $$e; done
