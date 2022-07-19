# SPDX-License-Identifier: GPL-2.0-or-later
MODULE_NAME := "qcom_uefivars"
MODULE_VERSION := "0.1"

KVERSION := "$(shell uname -r)"
KDIR := /lib/modules/$(KVERSION)/build
MDIR := /usr/src/$(MODULE_NAME)-$(MODULE_VERSION)

CHECKPATCH_OPTS := -f -q --no-tree
CHECKPATCH := $(KDIR)/scripts/checkpatch.pl $(CHECKPATCH_OPTS)


sources-dkms := dkms.conf
sources-dkms += Makefile

sources-c := $(shell find . -type f \( -name "*.c" -and -not -name "*.mod.c" \))
sources-h := $(shell find . -type f -name "*.h")
sources-Kbuild := $(shell find . -type f -name "Kbuild")

sources := $(sources-c) $(sources-h) $(sources-Kbuild) $(sources-dkms)


all:
	$(MAKE) -C $(KDIR) M=$(shell pwd) modules

clean:
	$(MAKE) -C $(KDIR) M=$(shell pwd) clean

%.check:
	@$(CHECKPATCH) $(basename $@) || true

check:
	@$(CHECKPATCH) $(sources-c) $(sources-h)

insmod:
	sudo insmod $(MODULE_NAME).ko

rmmod:
	sudo rmmod $(MODULE_NAME)

dkms-install:
	@for f in $(sources); do		\
		mkdir -p `dirname $(MDIR)/$$f`;	\
		cp -v $$f $(MDIR)/$$f;		\
	done
	dkms add $(MODULE_NAME)/$(MODULE_VERSION)
	dkms build $(MODULE_NAME)/$(MODULE_VERSION)
	dkms install $(MODULE_NAME)/$(MODULE_VERSION)

dkms-uninstall:
	modprobe -r $(MODULE_NAME) || true
	dkms uninstall $(MODULE_NAME)/$(MODULE_VERSION) || true
	dkms remove $(MODULE_NAME)/$(MODULE_VERSION) --all || true
	rm -rf $(MDIR)
