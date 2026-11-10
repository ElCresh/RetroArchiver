# RetroArchiver - tools for archiving floppies, CDs and DVDs
#
#   make          Win32 executables, run from XP upward
#   make native   local build, needed by the tests
#   make xp-tests unit.exe, fake_samdisk.exe, fake_dic.exe, fake_https.exe, settings_test.exe,
#                 tty_test.exe, fdcheck.exe
#                 for the XP VM (settings_test.exe also on the Windows host)
#   make test     generates the fixtures and runs all the suites
#   make setup    dist/RetroArchiver-Setup.exe, the installer for XP (NSIS 3);
#                 third-party programs in THIRDPARTY (default installer/third-party)
#   make clean

CC       ?= gcc
WINCC    ?= i686-w64-mingw32-gcc
CFLAGS   ?= -O2 -Wall
INC       = -Isrc
WINDEFS   = -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -D__USE_MINGW_ANSI_STDIO=1
WINFLAGS  = $(CFLAGS) $(INC) $(WINDEFS) -ffunction-sections -Wl,--gc-sections -s -static

# the network (net, tls, https over BearSSL, and iakeys and iaupload on top
# of them): the window uses it, dskrun and dskdiff do not. iarec.c, the
# record an upload leaves in the bag, needs no network and stays in CORE:
# the archive reads it
NET_C  = src/core/net.c src/core/tls.c src/core/https.c src/core/iakeys.c src/core/iaupload.c
CORE   = $(filter-out $(NET_C),$(wildcard src/core/*.c))
CORE_H = $(wildcard src/core/*.h)
UNIT   = $(wildcard tests/unit/*.c)
# the window's languages: portable C, tested with the core
LANG_C = src/gui/lang.c src/gui/arc_text.c
LANG_H = src/gui/lang.h src/gui/lang_en.h src/gui/arc_text.h src/core/messages.h src/core/messages_list.h
LANGS  = $(wildcard lang/*.lng)

# BearSSL (third_party/bearssl, unmodified): a static library for each
# compiler, with its own warnings. The whole src/ is compiled, the linker
# takes only the objects used and drops the unused functions. The system
# sources are chosen here: CryptGenRandom and GetSystemTimeAsFileTime on
# Windows, /dev/urandom and time() on the host; never RDRAND alone, which
# BearSSL would otherwise prefer to the system's generator.
BEARSSL    = third_party/bearssl
BR_SRC     = $(wildcard $(BEARSSL)/src/*.c $(BEARSSL)/src/*/*.c)
BR_HDR     = $(wildcard $(BEARSSL)/inc/*.h $(BEARSSL)/src/*.h)
BR_INC     = -I$(BEARSSL)/inc
BR_CFLAGS  = -O2 -ffunction-sections -fdata-sections -I$(BEARSSL)/inc -I$(BEARSSL)/src -DBR_RDRAND=0
# -mincoming-stack-boundary=2: Win32 threads get a stack aligned to 4 bytes, while the
# SSE2 and AES-NI code (chosen at run time with cpuid) wants 16: without it
# the TLS handshake crashed in a thread made with CreateThread
BR_WINDEFS = -DBR_USE_WIN32_RAND=1 -DBR_USE_WIN32_TIME=1 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -mincoming-stack-boundary=2
BR_HOSTDEFS = -DBR_USE_URANDOM=1 -DBR_USE_UNIX_TIME=1 -DBR_USE_GETENTROPY=0
BR_WOBJ    = $(patsubst $(BEARSSL)/src/%.c,build/bearssl/%.o,$(BR_SRC))
BR_HOBJ    = $(patsubst $(BEARSSL)/src/%.c,build/host/bearssl/%.o,$(BR_SRC))
WINAR     ?= i686-w64-mingw32-ar
AR        ?= ar

BIN = dskdiff.exe dskrun.exe RetroArchiver.exe
NAT = build/dskdiff build/dskrun build/unit build/fake_samdisk build/fake_dic build/fake_https
# Winsock, and CryptGenRandom (advapi32) for BearSSL
NETLIBS = -lws2_32 -ladvapi32

all: $(BIN) check-imports

check-imports: $(BIN)
	sh tests/check_imports.sh $(BIN)

dskdiff.exe: src/dskdiff.c $(CORE) $(CORE_H)
	$(WINCC) $(WINFLAGS) -o $@ src/dskdiff.c $(CORE)

dskrun.exe: src/dskrun.c $(CORE) $(CORE_H)
	$(WINCC) $(WINFLAGS) -o $@ src/dskrun.c $(CORE)

# the window: Windows only
GUI_C   = src/gui/settings.c src/gui/settings_dlg.c src/gui/about.c src/gui/winutil.c src/gui/box_edit_dlg.c src/gui/ia_dlg.c src/gui/images_dlg.c src/gui/imgdec.c
GUI     = $(GUI_C) src/gui/settings.h src/gui/dialogs.h src/gui/winutil.h src/gui/resource.h src/gui/imgdec.h $(LANG_C) $(LANG_H)
# the network too (Test the keys of the Archive.org tab); crypt32.dll, for
# the DPAPI of the keys, is not linked: settings.c loads it with
# LoadLibrary, so it stays out of the imports check_imports.sh reads
GUILIBS = -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32 -ladvapi32 -lws2_32

build/retroarchiver_res.o: src/gui/retroarchiver.rc src/gui/resource.h src/gui/retroarchiver.manifest src/gui/retroarchiver.ico | build
	i686-w64-mingw32-windres -I src/gui -O coff -o $@ src/gui/retroarchiver.rc

RetroArchiver.exe: src/gui/main.c $(GUI) build/retroarchiver_res.o $(CORE) $(CORE_H) $(NET_C) build/libbearssl.a
	$(WINCC) $(WINFLAGS) $(BR_INC) -municode -mwindows -o $@ src/gui/main.c $(GUI_C) $(LANG_C) \
		build/retroarchiver_res.o $(CORE) $(NET_C) build/libbearssl.a $(GUILIBS)

native: $(NAT)

build:
	mkdir -p build

build/dskdiff: src/dskdiff.c $(CORE) $(CORE_H) | build
	$(CC) $(CFLAGS) $(INC) -o $@ src/dskdiff.c $(CORE)

build/dskrun: src/dskrun.c $(CORE) $(CORE_H) | build
	$(CC) $(CFLAGS) $(INC) -o $@ src/dskrun.c $(CORE)

# RA_TEST_BUILD: https.c accepts http://127.0.0.1 (the fake server's plain port)
build/unit: $(UNIT) $(CORE) $(CORE_H) $(NET_C) $(LANG_C) $(LANG_H) tests/unit/unit.h build/host/libbearssl.a build/fake_samdisk build/fake_dic build/fake_https | build
	$(CC) $(CFLAGS) $(INC) $(BR_INC) -DRA_TEST_BUILD -Itests/unit -o $@ $(UNIT) $(CORE) $(NET_C) $(LANG_C) build/host/libbearssl.a -pthread

build/bearssl/%.o: $(BEARSSL)/src/%.c $(BR_HDR)
	@mkdir -p $(@D)
	@$(WINCC) $(BR_CFLAGS) $(BR_WINDEFS) -c -o $@ $<

build/host/bearssl/%.o: $(BEARSSL)/src/%.c $(BR_HDR)
	@mkdir -p $(@D)
	@$(CC) $(BR_CFLAGS) $(BR_HOSTDEFS) -c -o $@ $<

build/libbearssl.a: $(BR_WOBJ)
	@rm -f $@
	@$(WINAR) rcs $@ $^
	@echo "  $@: $(words $^) objects"

build/host/libbearssl.a: $(BR_HOBJ)
	@rm -f $@
	@$(AR) rcs $@ $^
	@echo "  $@: $(words $^) objects"

build/fake_samdisk: tests/fake_samdisk.c | build
	$(CC) $(CFLAGS) -o $@ $<

# core tests and fake SAMdisk for the XP VM
xp-tests: build/unit.exe build/fake_samdisk.exe build/fake_dic.exe build/fake_https.exe build/https_test.exe build/settings_test.exe build/tty_test.exe build/fdcheck.exe
	sh tests/check_imports.sh build/unit.exe build/fake_https.exe build/https_test.exe

build/unit.exe: $(UNIT) $(CORE) $(CORE_H) $(NET_C) $(LANG_C) $(LANG_H) tests/unit/unit.h build/libbearssl.a | build
	$(WINCC) $(WINFLAGS) $(BR_INC) -DRA_TEST_BUILD -Itests/unit -o $@ $(UNIT) $(CORE) $(NET_C) $(LANG_C) build/libbearssl.a $(NETLIBS)

# the fake HTTPS server of the network tests (certificates in tests/tls)
build/fake_https: tests/fake_https.c build/host/libbearssl.a | build
	$(CC) $(CFLAGS) $(BR_INC) -o $@ $< build/host/libbearssl.a -pthread

build/fake_https.exe: tests/fake_https.c build/libbearssl.a | build
	$(WINCC) $(WINFLAGS) $(BR_INC) -o $@ $< build/libbearssl.a $(NETLIBS)

# the network tests alone: on the XP VM they need only fake_https.exe and
# tests/tls, not the fixtures of the whole unit.exe. iaupload.c stays out:
# it sits on the whole core, and its tests (a bag to upload) are in unit.exe
NET_DEPS = src/core/strbuf.c src/core/fsu8.c src/core/sha256.c src/core/json.c
NET_ONLY = $(filter-out src/core/iaupload.c,$(NET_C))

build/https_test.exe: tests/xp/test_https_main.c tests/unit/test_https.c $(NET_ONLY) $(NET_DEPS) build/libbearssl.a | build
	$(WINCC) $(WINFLAGS) $(BR_INC) -DRA_TEST_BUILD -Itests/unit -o $@ \
		tests/xp/test_https_main.c tests/unit/test_https.c $(NET_ONLY) $(NET_DEPS) \
		build/libbearssl.a $(NETLIBS)

# the window settings in two ini files: Windows only (host or XP VM)
build/settings_test.exe: tests/xp/test_settings.c $(GUI) src/core/b64.c | build
	$(WINCC) $(WINFLAGS) -o $@ tests/xp/test_settings.c src/gui/settings.c src/core/b64.c -lshell32

# the hidden console of runner_run_tty: the test detaches from the console, like the GUI
build/tty_test.exe: tests/xp/test_tty.c src/core/runner.c src/core/runner.h src/core/strbuf.c | build
	$(WINCC) $(WINFLAGS) -o $@ tests/xp/test_tty.c src/core/runner.c src/core/strbuf.c

# probe for the automatic mode: is a floppy in the drive? (fdrawcmd, only on the real PC)
build/fdcheck.exe: tests/xp/fdcheck.c | build
	$(WINCC) $(WINFLAGS) -o $@ $<

build/fake_samdisk.exe: tests/fake_samdisk.c | build
	$(WINCC) $(WINFLAGS) -o $@ $<

build/fake_dic: tests/fake_dic.c | build
	$(CC) $(CFLAGS) -o $@ $<

build/fake_dic.exe: tests/fake_dic.c | build
	$(WINCC) $(WINFLAGS) -o $@ $<

test: native
	rm -rf tests/tmp_unit tests/tmp_session
	cd tests && python3 -I gen_edsk.py && python3 -I gen_dic.py && ../build/unit && ./run_tests.sh && ./golden.sh && python3 -I run_session_tests.py && python3 -I test_third_party.py && sh check_core_texts.sh
	@# the window settings and the hidden console are Windows only:
	@# tested here if WSL can run .exe files, otherwise on the VM (make xp-tests)
	@if [ -e /proc/sys/fs/binfmt_misc/WSLInterop ]; then \
		$(MAKE) -s --no-print-directory build/settings_test.exe build/tty_test.exe build/fake_samdisk.exe && \
		cd build && ./settings_test.exe && ./tty_test.exe fake_samdisk.exe ../tests/fixtures | tee tty_test.txt && \
		grep -q "^tty: 0 failed" tty_test.txt; \
	else echo "settings_test.exe and tty_test.exe skipped: Windows needed (WSL)"; fi

# ---- the installer ----
# Third-party programs are not in the repo: listed in installer/third-party.txt,
# they are downloaded by hand into THIRDPARTY. third_party.py checks their hashes and
# stops, saying what is missing and where to download it from.
THIRDPARTY ?= installer/third-party
MAKENSIS   ?= makensis
# the version is changed only in src/core/version.h (YYMMDD of the release)
VERSION    := $(shell sed -n 's/^.define RA_VERSION "\(.*\)"/\1/p' src/core/version.h)
VIVERSION  := $(shell echo $(VERSION) | awk '{printf "%d.%d.%d.0", substr($$0,1,2), substr($$0,3,2), substr($$0,5,2)}')
SETUPTXT    = build/setup/README.txt build/setup/setup.txt build/setup/procedure.txt \
              build/setup/THIRD-PARTY-NOTICES.txt $(patsubst lang/%,build/setup/lang/%,$(LANGS)) \
              build/setup/protection-signatures.txt build/setup/LICENSE.txt \
              build/setup/BearSSL-LICENSE.txt

# --version lets third_party.py say if the CA bundle (cacert.pem) has grown
# old next to this release; publish/publish.sh release makes it an error
setup: $(BIN) check-imports $(SETUPTXT)
	python3 -I installer/third_party.py installer/third-party.txt $(THIRDPARTY) build/third-party --version $(VERSION)
	mkdir -p dist
	$(MAKENSIS) -V2 -INPUTCHARSET UTF8 -DROOT=$(CURDIR) -DVERSION=$(VERSION) -DVIVERSION=$(VIVERSION) \
		-DOUTFILE=$(CURDIR)/dist/RetroArchiver-Setup.exe installer/retroarchiver.nsi
	@ls -l dist/RetroArchiver-Setup.exe

# Markdown files as text for XP's Notepad: UTF-8 with BOM, CRLF line endings
build/setup:
	mkdir -p build/setup

build/setup/README.txt: README.md | build/setup
	{ printf '\357\273\277'; sed 's/$$/\r/' $<; } > $@

build/setup/procedure.txt: docs/procedure.md | build/setup
	{ printf '\357\273\277'; sed 's/$$/\r/' $<; } > $@

build/setup/%.txt: docs/%.md | build/setup
	{ printf '\357\273\277'; sed 's/$$/\r/' $<; } > $@

# the window's languages, also readable in XP's Notepad: BOM and CRLF (lang.c accepts both)
build/setup/lang/%.lng: lang/%.lng
	mkdir -p build/setup/lang
	{ printf '\357\273\277'; sed 's/$$/\r/' $<; } > $@

# the copy protection signatures, editable with XP's Notepad: ASCII, CRLF
build/setup/protection-signatures.txt: installer/protection-signatures.txt | build/setup
	sed 's/\r*$$/\r/' $< > $@

# RetroArchiver's license, the GPL text as the FSF publishes it: ASCII, CRLF
build/setup/LICENSE.txt: LICENSE | build/setup
	sed 's/\r*$$/\r/' $< > $@

# BearSSL is compiled into RetroArchiver.exe, so it has no line in
# installer/third-party.txt: its licence goes to the setup from here
build/setup/BearSSL-LICENSE.txt: installer/licenses/BearSSL-LICENSE.txt | build/setup
	sed 's/\r*$$/\r/' $< > $@

# in English and ASCII only: no BOM, also for the installer page
build/setup/THIRD-PARTY-NOTICES.txt: THIRD-PARTY-NOTICES.txt | build/setup
	sed 's/\r*$$/\r/' $< > $@

clean:
	rm -rf build tests/fixtures tests/tmp_unit tests/tmp_session $(BIN) dist

.PHONY: all native test clean check-imports xp-tests setup
