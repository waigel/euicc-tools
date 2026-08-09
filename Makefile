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

# `euicc card` -- the LPA role of SGP.22, vendored so this binary can ask a
# real card what it is. euicc-lpa in turn stands on euicc-rsp for crypto,
# PKI and the generated codec; euicc-rsp's own dist/ duplicates almost the
# whole asn1c runtime and the PKIX types the schema's own dist already
# carries (BER/DER codecs, INTEGER, OCTET_STRING, Certificate,
# SubjectKeyIdentifier...); see build/libeuicc-full.a below for how two
# copies of that coexist in one binary without a link error.
LPA      := $(VENDOR)/euicc-lpa
RSP      := $(LPA)/vendor/euicc-rsp
LPA_LIB  := $(LPA)/liblpa.a
RSP_LIB  := $(RSP)/librsp.a
RSP_DIST := $(RSP)/dist
RSP_MBED := $(RSP)/vendor/mbedtls
RSP_MBED_LIBS := $(RSP_MBED)/library/libmbedcrypto.a $(RSP_MBED)/library/libmbedx509.a

# PC/SC: macOS ships it as a framework, Linux needs pcsc-lite. The same
# split euicc-lpa's own Makefile makes, repeated here because this binary
# now links straight to a reader too, not only through liblpa.a. euicc-rsp
# no longer builds anything PC/SC -- the card side, and the link flags it
# needs, moved to euicc-lpa with the rest of it.
ifeq ($(shell uname -s),Darwin)
RSP_PCSC_LIBS := -framework PCSC
else
RSP_PCSC_LIBS := $(shell pkg-config --libs libpcsclite 2>/dev/null || echo -lpcsclite)
endif

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

INC := -Isrc -Ibuild -I$(VN)/include -I$(VN)/src -I$(SKELDIR) -I$(LPA)/include -I$(RSP)/include $(XML_CFLAGS)
# The schema source, so `euicc schema` can say where a type is declared. Only
# a location: the schema itself is read from asn1c's descriptors and never
# from this file.
ASN     := $(SCHEMA)/profile-3.4.1.asn

# The number is what the language server compares against its minimum, so it
# moves when the CLI's surface does. The commit is for a bug report, and a
# build outside a checkout says so instead of inventing one. The SHA is no
# longer a -D here: see build/gitsha.h below for why.
VERSION := 1.1

DEF := -DEUICC_RULES_DIR='"$(abspath $(RULES))"' \
       -DEUICC_SKEL_DIR='"$(SKEL)"' \
       -DEUICC_SCHEMA_FILE='"$(abspath $(ASN))"' \
       -DEUICC_VERSION='"$(VERSION)"'

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

OWN_SRCS := src/main.c src/schematron.c src/diff.c src/schema.c src/format.c src/card.c
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

# GITSHA used to be evaluated once, at Make's parse time, and baked in with
# -DEUICC_GITSHA; that value never changed again for the rest of this
# invocation, so a commit that touched no source left the binary reporting
# the PREVIOUS commit -- exactly the confusion the SHA exists to prevent,
# since it is what points a bug report at the build that produced it.
#
# gitsha-force is phony, so this recipe runs on every build and asks git
# fresh each time -- but it only overwrites build/gitsha.h when the SHA text
# actually changed, so a build where HEAD did not move leaves the header's
# mtime alone and does not force euicc to relink for nothing.
.PHONY: gitsha-force
gitsha-force:

build/gitsha.h: gitsha-force
	@mkdir -p build
	@sha="$$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"; \
	 new='#define EUICC_GITSHA "'"$$sha"'"'; \
	 if [ ! -f $@ ] || [ "$$(cat $@)" != "$$new" ]; then \
	     echo "$$new" > $@; \
	 fi

# ---- euicc-lpa --------------------------------------------------------------
# euicc-lpa builds its own library (the transport, the ES10 command layer,
# and the read-only card commands), which in turn builds euicc-rsp for its
# crypto, its PKI and its generated codec (from rsp-2.5.asn) -- asked for by
# name, the same way this Makefile asks euicc-schema's submodule to build
# the SAIP codec above. Passed the asn1c this Makefile already built, so
# the generated runtime -- ber_decoder.c, INTEGER.c, OCTET_STRING.c, the
# PKIX types both projects need -- comes out the same as euicc-schema's own
# dist. tools/check-codec-agreement (wired in below as the codec-agreement
# prerequisite) is what actually checks that, file by file, on every build;
# it names five specific files it lets differ and says why each is safe --
# see the script itself. That is what makes build/libeuicc-full.a below
# possible: two copies of the same code, not two different ones that merely
# happen to agree.
#
# $(RSP_LIB) used to depend only on the asn1c binary, so once it existed
# nothing here ever asked euicc-rsp to rebuild it again: touching
# vendor/euicc-lpa/vendor/euicc-rsp/src/rsp_es10.c or its include/rsp.h and
# running "make -n euicc" showed nothing to do. The realistic way that
# bites is a submodule pointer bump -- checkout refreshes the sources,
# liblpa.a keeps its old mtime, and the binary links the previous library.
# lpa-lib-force makes this recipe run on every build, delegating the actual
# staleness decision to euicc-lpa's own Makefile (which in turn delegates
# librsp.a's own staleness to euicc-rsp's), which tracks its sources AND its
# headers in its %.o rule. `ar` there only touches liblpa.a's mtime when a
# member object actually changed, so a build with nothing to do here stays
# cheap: the recipe runs, finds nothing stale, and the mtime -- and
# everything downstream of it -- is left alone.
.PHONY: lpa-lib-force
lpa-lib-force:

$(LPA_LIB): $(ASN1C)/asn1c/asn1c lpa-lib-force
	@test -e $(RSP)/vendor/mbedtls/.git || { \
	    echo "the euicc-lpa submodule's own submodules are missing:" >&2; \
	    echo "  git -C $(LPA) submodule update --init --recursive" >&2; \
	    exit 1; }
	$(MAKE) -C $(LPA) ASN1C="$(abspath $(ASN1C)/asn1c/asn1c)" SKELDIR="$(abspath $(SKELDIR))"

# euicc-rsp's dist/ (rsp-2.5.asn's own types: EUICCInfo2, GetEuiccDataResponse,
# ProfileInfoListResponse, and the rest) is compiled as a side effect of the
# rule above, into dist/*.o sitting right next to librsp.a's own members --
# neither is rebuilt here, both are just gathered.
#
# Almost everything in that dist/ duplicates a file euicc-schema's own dist
# already provides under the same name: the generic asn1c runtime (every
# schema needs BER/DER, INTEGER, OCTET_STRING, ...) and the PKIX types
# (Certificate, SubjectKeyIdentifier, ...) that both a profile package and an
# RSP handshake reference. Two loose .o files defining the same strong symbol
# is a link error regardless of whether their content agrees -- but a static
# archive is not: the linker only pulls a member out of one to resolve a
# symbol still outstanding. build/gen/*.o (euicc-schema's dist, linked as
# plain .o below, always included) resolves those shared symbols first; the
# archive built here carries a second copy of every one of them, and the
# linker simply never has a reason to reach for it. Only the members nothing
# else defines -- euicc-rsp's own dist/ types, and everything liblpa.a
# itself carries -- actually get pulled in.
#
# liblpa.a's members are extracted into the same directory, after
# librsp.a's, so the LPA's own objects (rsp_pcsc.o, rsp_es10.o, ...) sit
# alongside librsp.a's and go into the one archive below with them. A
# member name collision between the two archives would silently overwrite
# one during extraction, but there is none: liblpa.a and librsp.a are built
# from disjoint source trees.
# All three sources, not just the first. This recipe extracts from
# librsp.a and copies euicc-rsp's dist/ as well as unpacking liblpa.a,
# and it used to depend on liblpa.a alone. So a change confined to
# euicc-rsp rebuilt librsp.a, left liblpa.a's mtime untouched, and make
# declared this stamp up to date: the old object stayed in the archive
# and the binary linked code that had already been overwritten on disk.
# That cost four separate debugging detours in one session, each one
# presenting as "my edit had no effect" rather than as a stale build.
build/rsp-objs/.stamp: $(LPA_LIB) $(RSP_LIB)
	rm -rf build/rsp-objs
	mkdir -p build/rsp-objs
	cd build/rsp-objs && ar x $(abspath $(RSP_LIB))
	cd build/rsp-objs && ar x $(abspath $(LPA_LIB))
	cp $(RSP_DIST)/*.o build/rsp-objs/
	@touch $@

build/libeuicc-full.a: build/rsp-objs/.stamp
	rm -f $@
	ar rcs $@ build/rsp-objs/*.o

# The archive trick above depends on build/gen's copy of the shared runtime
# and PKIX types agreeing with the copy buried in build/libeuicc-full.a --
# see the comment where LPA_LIB is defined for why that is expected to hold.
# tools/check-codec-agreement is what actually checks it, comparing
# $(DIST) (euicc-schema's dist, the source build/gen/*.o is compiled from)
# against $(RSP_DIST) file by file. A prerequisite of $(LPA_LIB) rather than
# of build/rsp-objs/.stamp or build/libeuicc-full.a themselves, so a
# divergence is reported before anything is archived, not after.
.PHONY: codec-agreement
codec-agreement: $(DIST)/ProfileElement.h $(LPA_LIB)
	@./tools/check-codec-agreement $(DIST) $(RSP_DIST)

# ---- the binary ------------------------------------------------------------

# Makefile is a dependency on purpose: VERSION is compiled in via -D, so an
# edit here has to rebuild -- without this, bumping VERSION produced a
# binary that still answered with the old one, which is precisely the
# confusion `euicc version` exists to prevent. GITSHA takes the same care
# through build/gitsha.h instead, since it changes on every commit rather
# than on a Makefile edit.
euicc: $(OWN_SRCS) src/euicc.h build/vn_annotations.c build/gen/.stamp \
       build/libeuicc-full.a build/gitsha.h codec-agreement Makefile
	@test -n "$(XML_LIBS)" || { \
	    echo "libxml2 and libxslt are needed; install them and try again" >&2; \
	    exit 1; }
	$(CC) $(ALL_CFLAGS) -idirafter $(abspath $(DIST)) \
	    $(OWN_SRCS) $(VN_SRCS) build/vn_annotations.c build/gen/*.o \
	    build/libeuicc-full.a $(RSP_MBED_LIBS) $(RSP_PCSC_LIBS) \
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
	$(MAKE) -C $(LPA) clean 2>/dev/null || true
	$(MAKE) -C $(RSP) clean 2>/dev/null || true
