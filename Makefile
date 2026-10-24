# RetroArchiver - tools for archiving floppies, CDs and DVDs
#
#   make          Win32 executables, run from XP upward
#   make native   local build, needed by the tests
#   make xp-tests unit.exe, fake_samdisk.exe, fake_dic.exe, settings_test.exe, tty_test.exe, fdcheck.exe
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

CORE   = $(wildcard src/core/*.c)
CORE_H = $(wildcard src/core/*.h)
UNIT   = $(wildcard tests/unit/*.c)
# the window's languages: portable C, tested with the core
LANG_C = src/gui/lang.c
LANG_H = src/gui/lang.h src/gui/lang_en.h
LANGS  = $(wildcard lang/*.lng)

BIN = dskdiff.exe dskrun.exe RetroArchiver.exe
NAT = build/dskdiff build/dskrun build/unit build/fake_samdisk build/fake_dic

all: $(BIN) check-imports

check-imports: $(BIN)
	sh tests/check_imports.sh $(BIN)

dskdiff.exe: src/dskdiff.c $(CORE) $(CORE_H)
	$(WINCC) $(WINFLAGS) -o $@ src/dskdiff.c $(CORE)

dskrun.exe: src/dskrun.c $(CORE) $(CORE_H)
	$(WINCC) $(WINFLAGS) -o $@ src/dskrun.c $(CORE)

# the window: Windows only
GUI_C   = src/gui/settings.c src/gui/settings_dlg.c src/gui/about.c src/gui/winutil.c
GUI     = $(GUI_C) src/gui/settings.h src/gui/dialogs.h src/gui/winutil.h src/gui/resource.h $(LANG_C) $(LANG_H)
GUILIBS = -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32 -ladvapi32

build/retroarchiver_res.o: src/gui/retroarchiver.rc src/gui/resource.h src/gui/retroarchiver.manifest src/gui/retroarchiver.ico | build
	i686-w64-mingw32-windres -I src/gui -O coff -o $@ src/gui/retroarchiver.rc

RetroArchiver.exe: src/gui/main.c $(GUI) build/retroarchiver_res.o $(CORE) $(CORE_H)
	$(WINCC) $(WINFLAGS) -municode -mwindows -o $@ src/gui/main.c $(GUI_C) $(LANG_C) build/retroarchiver_res.o $(CORE) $(GUILIBS)

native: $(NAT)

build:
	mkdir -p build

build/dskdiff: src/dskdiff.c $(CORE) $(CORE_H) | build
	$(CC) $(CFLAGS) $(INC) -o $@ src/dskdiff.c $(CORE)

build/dskrun: src/dskrun.c $(CORE) $(CORE_H) | build
	$(CC) $(CFLAGS) $(INC) -o $@ src/dskrun.c $(CORE)

build/unit: $(UNIT) $(CORE) $(CORE_H) $(LANG_C) $(LANG_H) tests/unit/unit.h build/fake_samdisk build/fake_dic | build
	$(CC) $(CFLAGS) $(INC) -Itests/unit -o $@ $(UNIT) $(CORE) $(LANG_C)

build/fake_samdisk: tests/fake_samdisk.c | build
	$(CC) $(CFLAGS) -o $@ $<

# core tests and fake SAMdisk for the XP VM
xp-tests: build/unit.exe build/fake_samdisk.exe build/fake_dic.exe build/settings_test.exe build/tty_test.exe build/fdcheck.exe

build/unit.exe: $(UNIT) $(CORE) $(CORE_H) $(LANG_C) $(LANG_H) tests/unit/unit.h | build
	$(WINCC) $(WINFLAGS) -Itests/unit -o $@ $(UNIT) $(CORE) $(LANG_C)

# the window settings in two ini files: Windows only (host or XP VM)
build/settings_test.exe: tests/xp/test_settings.c $(GUI) | build
	$(WINCC) $(WINFLAGS) -o $@ tests/xp/test_settings.c src/gui/settings.c -lshell32

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
	cd tests && python3 -I gen_edsk.py && python3 -I gen_dic.py && ../build/unit && ./run_tests.sh && ./golden.sh && python3 -I run_session_tests.py && python3 -I test_third_party.py
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
              build/setup/protection-signatures.txt

setup: $(BIN) check-imports $(SETUPTXT)
	python3 -I installer/third_party.py installer/third-party.txt $(THIRDPARTY) build/third-party
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

# in English and ASCII only: no BOM, also for the installer page
build/setup/THIRD-PARTY-NOTICES.txt: THIRD-PARTY-NOTICES.txt | build/setup
	sed 's/\r*$$/\r/' $< > $@

clean:
	rm -rf build tests/fixtures tests/tmp_unit tests/tmp_session $(BIN) dist

.PHONY: all native test clean check-imports xp-tests setup
