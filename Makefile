# euicc -- one command to build and check eUICC profile packages.
#
# Everything it needs is a submodule. The build compiles asn1c, generates the
# codec from the schema, and links that together with asn1c-vn and libxslt into
# a single binary. Nothing has to be installed system-wide except libxml2 and
# libxslt, which macOS and every Linux distribution ship.
#
#     make            the binary
#     make check      the tests
#     make clean      the objects, keeping the generated codec
#     make distclean  everything the build produced

CC      ?= cc
CFLAGS  ?= -O2 -g
STD     := -std=c99
# An implicit declaration is the bug above waiting to happen again, so it is
# an error and not a warning that scrolls past.
WARN    := -Wall -Wextra -Wno-unused-parameter \
           -Werror=implicit-function-declaration -Werror=int-conversion

VENDOR  := vendor
SCHEMA  := $(VENDOR)/euicc-schema
VN      := $(SCHEMA)/asn1c-vn
ASN1C   := $(SCHEMA)/asn1c
RULES   := $(VENDOR)/saip-validator/rules
DIST    := $(SCHEMA)/dist

# asn1c copies only the skeletons that a schema uses, so a generated directory
# is missing headers that asn1c-vn includes: RELATIVE-OID.h among them. The
# skeleton directory of the asn1c source has the complete set, and it is a
# submodule here, so nothing has to be installed to find it.
SKELDIR := $(ASN1C)/skeletons

# The ISO Schematron transforms. lxml ships them, and so does any distribution
# package of Schematron. The path is compiled in as a default and -–skel
# overrides it.
SKEL ?= $(shell python3 -c "import lxml.isoschematron as i,os;\
          print(os.path.join(os.path.dirname(i.__file__),'resources','xsl','iso-schematron-xslt1'))" 2>/dev/null)

XML_CFLAGS := $(shell xml2-config --cflags 2>/dev/null) \
              $(shell pkg-config --cflags libxslt 2>/dev/null)
XML_LIBS   := $(shell xml2-config --libs 2>/dev/null) \
              $(shell pkg-config --libs libxslt 2>/dev/null || echo -lxslt)

INC := -Isrc -I$(VN)/include -I$(VN)/src -I$(SKELDIR) $(XML_CFLAGS)
# The schema source, so `euicc schema` can say where a type is declared. Only
# a location: the schema itself is read from asn1c's descriptors and never
# from this file.
ASN     := $(SCHEMA)/profile-3.4.1.asn

# The number is what the language server compares against its minimum, so it
# moves when the CLI's surface does. The commit is for a bug report, and a
# build outside a checkout says so instead of inventing one.
VERSION := 1.1
GITSHA  := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

DEF := -DEUICC_RULES_DIR='"$(abspath $(RULES))"' \
       -DEUICC_SKEL_DIR='"$(SKEL)"' \
       -DEUICC_SCHEMA_FILE='"$(abspath $(ASN))"' \
       -DEUICC_VERSION='"$(VERSION)"' -DEUICC_GITSHA='"$(GITSHA)"'

# -std=c99 makes glibc hide everything younger than C89: strdup, getline,
# glob and timegm lose their declarations, an implicit declaration returns
# int, and an int truncates a 64-bit pointer. On Linux that was a segfault in
# `euicc check`. macOS never showed it, because _DARWIN_C_SOURCE reveals
# everything there; _DEFAULT_SOURCE is the glibc counterpart.
EXTRA := -D_DEFAULT_SOURCE
ifeq ($(shell uname -s),Darwin)
# asn1c's GeneralizedTime.c needs struct tm and timegm.
EXTRA += -D_DARWIN_C_SOURCE
endif

ALL_CFLAGS = $(STD) $(WARN) $(CFLAGS) $(EXTRA) $(INC) $(DEF)

OWN_SRCS := src/main.c src/schematron.c src/diff.c src/schema.c src/format.c
VN_SRCS  := $(wildcard $(VN)/src/*.c)
GEN_SRCS  = $(filter-out $(DIST)/converter-example.c, $(wildcard $(DIST)/*.c))

.PHONY: all check clean distclean codec install uninstall

all: euicc

# ---- the codec -------------------------------------------------------------
# Generated once from the schema. The submodule build knows how, including the
# PKIX modules that the asn1c repository does not ship.

$(ASN1C)/asn1c/asn1c:
	@# .git is a file in a submodule, not a directory.
	@test -e $(ASN1C)/.git || { \
	    echo "the submodules are missing: git submodule update --init --recursive" >&2; \
	    exit 1; }
	cd $(ASN1C) && { test -f configure || autoreconf -iv; } && ./configure && $(MAKE)

$(DIST)/ProfileElement.h: $(ASN1C)/asn1c/asn1c
	$(MAKE) -C $(SCHEMA) dist ASN1C="$(abspath $(ASN1C)/asn1c/asn1c)"

codec: $(DIST)/ProfileElement.h

# Identifiers that asn1c parses and does not keep: INTEGER named numbers and
# BIT STRING named bits. Without the table a reader rejects the identifiers
# that reference tools write, and a writer prints numbers where the
# specification prints names.
build/vn_annotations.c: $(DIST)/ProfileElement.h
	@mkdir -p build
	$(MAKE) -C $(VN) vn-annotate SKELDIR="$(abspath $(SKELDIR))"
	$(VN)/vn-annotate $(abspath $(DIST)) > $@

# asn1c's own output is compiled with warnings off: it is generated code, and
# it is not ours to correct. -idirafter, never -I, because the PKIX modules
# define an ASN.1 type Time, whose Time.h hides the system <time.h> on a
# case-insensitive filesystem.
build/gen/.stamp: $(DIST)/ProfileElement.h
	rm -rf build/gen
	@mkdir -p build/gen
	cd build/gen && $(CC) $(STD) $(CFLAGS) $(EXTRA) -w \
	    -idirafter $(abspath $(DIST)) -I$(abspath $(SKELDIR)) \
	    -c $(abspath $(GEN_SRCS))
	@touch $@

# ---- the binary ------------------------------------------------------------

# Makefile is a dependency on purpose: VERSION and GITSHA are compiled in, so
# an edit here has to rebuild -- without this, bumping VERSION produced a
# binary that still answered with the old one, which is precisely the
# confusion `euicc version` exists to prevent.
euicc: $(OWN_SRCS) src/euicc.h build/vn_annotations.c build/gen/.stamp Makefile
	@test -n "$(XML_LIBS)" || { \
	    echo "libxml2 and libxslt are needed; install them and try again" >&2; \
	    exit 1; }
	$(CC) $(ALL_CFLAGS) -idirafter $(abspath $(DIST)) \
	    $(OWN_SRCS) $(VN_SRCS) build/vn_annotations.c build/gen/*.o \
	    -o $@ $(XML_LIBS) -lm

check: euicc
	./tests/run-tests

# The rule set and the Schematron transforms are compiled in as absolute paths,
# so an installed binary reads them where they are. Moving the checkout breaks
# that; --rules and --skel override it.
PREFIX ?= /usr/local

install: euicc
	@mkdir -p $(DESTDIR)$(PREFIX)/bin
	install -m 755 euicc $(DESTDIR)$(PREFIX)/bin/euicc
	@echo "installed $(DESTDIR)$(PREFIX)/bin/euicc"
	@echo "rules:      $(abspath $(RULES))"
	@echo "transforms: $(SKEL)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/euicc

clean:
	rm -rf build euicc

distclean: clean
	$(MAKE) -C $(SCHEMA) distclean 2>/dev/null || true
