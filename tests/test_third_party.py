"""Tests of installer/third_party.py with fake third-party files (no downloads).

    cd tests && python3 -I test_third_party.py
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
THIRD_PARTY = os.path.join(HERE, "..", "installer", "third_party.py")
fails = 0


def ok(cond, what):
    global fails
    print("  %s %s" % ("ok  " if cond else "FAIL", what))
    if not cond:
        fails += 1


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def run(list_path, src, dest):
    p = subprocess.run([sys.executable, "-I", THIRD_PARTY, list_path, src, dest],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    tmp = tempfile.mkdtemp(prefix="third_party_")
    try:
        src = os.path.join(tmp, "scaricati")
        dest = os.path.join(tmp, "build")
        os.makedirs(src)
        # a zip like DiscImageCreator's: license at the top, program in a folder
        z = os.path.join(src, "prog.zip")
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("LICENSE", "licenza")
            f.writestr("Release/prog.exe", "exe")
            f.writestr("Release/Doc/note & varie.txt", "doc")
            f.writestr("Altro/escluso.txt", "no")
        inst = os.path.join(src, "Setup.exe")
        with open(inst, "wb") as f:
            f.write(b"MZ installer")
        # the license texts sit next to the list, in licenses/
        os.makedirs(os.path.join(tmp, "licenses"))
        with open(os.path.join(tmp, "licenses", "prog-LICENSE.txt"), "wb") as f:
            f.write(b"MIT License\n\nCopyright\n")
        list_path = os.path.join(tmp, "third-party.txt")
        with open(list_path, "w", encoding="utf-8") as f:
            f.write("# commento\n\n")
            f.write("prog|Programma|1.0|prog.zip|%s|https://example.org/prog.zip|Release|MIT|prog-LICENSE.txt\n" % sha(z))
            f.write("drv|Driver|2.0|Setup.exe|%s|https://example.org/Setup.exe|-|freeware|-\n" % sha(inst))

        code, out = run(list_path, src, dest)
        ok(code == 0, "right files: exits with 0")
        ok(os.path.isfile(os.path.join(dest, "prog", "prog.exe")), "zip: the root becomes the folder")
        ok(os.path.isfile(os.path.join(dest, "prog", "LICENSE")), "zip: files at the top of the zip are included")
        ok(os.path.isfile(os.path.join(dest, "prog", "Doc", "note & varie.txt")), "zip: subfolders are kept")
        ok(not os.path.exists(os.path.join(dest, "prog", "escluso.txt")) and
           not os.path.exists(os.path.join(dest, "prog", "Altro")), "zip: nothing outside the root")
        ok(os.path.isfile(os.path.join(dest, "drv", "Setup.exe")), "installer: copied as is")
        nsh = open(os.path.join(dest, "third_party.nsh"), encoding="utf-8").read()
        ok('!define T_prog_VERSION "1.0"' in nsh, "nsh: version")
        ok('Delete "${DIR}\\Doc\\note & varie.txt"' in nsh, "nsh: the macro removes the files")
        ok(nsh.index('RMDir "${DIR}\\Doc"') < nsh.index('RMDir "${DIR}"\n'), "nsh: subfolders first")
        lic = os.path.join(dest, "licenses", "prog-LICENSE.txt")
        ok(os.path.isfile(lic) and open(lic, "rb").read() == b"MIT License\r\n\r\nCopyright\r\n",
           "licence: in licenses/, with CRLF line endings")
        ok(os.path.isfile(os.path.join(dest, "prog", "prog-LICENSE.txt")), "licence: also next to the program")
        ok('Delete "${DIR}\\prog-LICENSE.txt"' in nsh, "licence: the macro removes it")
        ok(os.listdir(os.path.join(dest, "drv")) == ["Setup.exe"] and
           sorted(os.listdir(os.path.join(dest, "licenses"))) == ["prog-LICENSE.txt"],
           "licence '-': no file")
        ok('!define T_prog_LICENSE_FILE "prog-LICENSE.txt"' in nsh, "nsh: the licence file name")

        os.rename(os.path.join(tmp, "licenses", "prog-LICENSE.txt"), os.path.join(tmp, "licenses", "altro.txt"))
        code, out = run(list_path, src, dest)
        ok(code == 1 and "licence text is missing" in out, "missing licence text: stops")
        os.rename(os.path.join(tmp, "licenses", "altro.txt"), os.path.join(tmp, "licenses", "prog-LICENSE.txt"))

        with open(inst, "ab") as f:
            f.write(b"!")
        os.remove(z)
        code, out = run(list_path, src, dest)
        ok(code == 1, "missing or changed file: exits with 1")
        ok("prog.zip is missing" in out and "https://example.org/prog.zip" in out, "says what is missing and where from")
        ok("Setup.exe has a different hash" in out, "says which hash is wrong")

        with open(list_path, "a", encoding="utf-8") as f:
            f.write("rotta|solo tre|campi\n")
        code, out = run(list_path, src, dest)
        ok(code != 0 and "9 fields" in out, "bad line in the list: stops")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print()
    print("  third_party.py: %s" % ("all green." if not fails else "%d checks failed." % fails))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
