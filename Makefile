SUBDIRS = hello scull
#SUBDIRS =  misc-progs misc-modules \
           skull scull scullc scullp scullv sbull spull snull\
	   short shortprint pci simple usb allocator
all: subdirs

subdirs:
	for n in $(SUBDIRS); do $(MAKE) -C /lib/modules/$(shell uname -r)/build M=${PWD}/$$n modules || exit 1; done

clean:
	rm -f *.o *~
	for n in $(SUBDIRS); do $(MAKE) -C $$n clean; done

checkthem:
	for n in $(SUBDIRS); do $(MAKE) -C $$n checkthem; done

check:
	for n in $(SUBDIRS); do $(MAKE) -C $$n check; done

objs:
	for n in $(SUBDIRS); do $(MAKE) -C $$n objs; done

run:
	exit
