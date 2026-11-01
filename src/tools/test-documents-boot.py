#!/usr/bin/env python3
"""Documents on the real system: home's file manager opens each sample with
view (its one candidate), on the guest's DRM screen through process 1, and
view's keys act on the picture; then a text file, chosen for edit in the
chooser (view and edit open text), is changed and saved, and term shows the
file's new text and its owner. Run by test-root-boot.sh with
ROOT_BOOT_DRIVER=test-documents-boot.py, ROOT_BOOT_FILES (a.pdf, b.md, c.epub,
d.djvu, e.cbz, notes.txt) and ROOT_BOOT_RC (program_view, program_edit).
  test-documents-boot.py TEST_DIRECTORY TIMEOUT_SECONDS
"""
import importlib.util
import json
from pathlib import Path
import re
import sys

_spec = importlib.util.spec_from_file_location("root_boot", Path(__file__).with_name("test-root-boot.py"))
root_boot = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(root_boot)

# The samples in Documents, in the file manager's order, the format each
# should open as, and whether the chooser asks first: Markdown is plain text
# too, which edit opens (view is first, rc.conf's order).
DOCUMENTS = [("a.pdf", "PDF", False), ("b.md", "Markdown", True), ("c.epub", "EPUB", False),
             ("d.djvu", "DjVu", False), ("e.cbz", "Comic book", False)]


def opened(guest, name, fmt):
    guest.wait_for(br"home: open \S+/" + re.escape(name).encode() + br" \([^)]*\) with program:view")
    guest.wait_for(br"view: opened \S+/" + re.escape(name).encode() + br": " + re.escape(fmt).encode() +
                   br", [1-9][0-9]* pages")
    guest.pump(2.0)  # the first pages rendered and shown


def closed(guest, name):
    guest.key("ctrl", "q")
    guest.wait_for(br"home: ended /usr/local/bin/view \S+/" + re.escape(name).encode() + br" 0")
    guest.pump(0.5)


def main():
    if len(sys.argv) != 3:
        raise SystemExit("Usage: test-documents-boot.py TEST_DIRECTORY TIMEOUT_SECONDS")
    out = Path(sys.argv[1]).resolve()
    guest = None
    shots, failures = {}, []
    try:
        guest = root_boot.Guest(out, float(sys.argv[2]))
        guest.wait_for(br"init: locking")
        guest.wait_for(br"init: started lock \(pid [0-9]+\)")
        guest.pump(1.5)
        guest.type(root_boot.PASSWORD)
        guest.key("ret")
        guest.wait_for(br"init: unlocked")
        guest.wait_for(br"home> ")
        guest.key("ret")  # files, home's first entry
        guest.pump(1.0)
        guest.key("ret")  # Documents, the home's only folder
        guest.pump(1.0)
        shots["files"] = guest.shot("files")
        for i, (name, fmt, asks) in enumerate(DOCUMENTS):
            if i:
                guest.key("down")
            guest.key("ret")
            if asks:
                guest.pump(1.0)
                shots["chooser-" + name] = guest.shot("chooser-" + name)
                guest.key("1")
                guest.key("ret")
            opened(guest, name, fmt)
            shots[name] = guest.shot("view-" + name)
            if name == "a.pdf":
                guest.key("ctrl", "equal")  # zoom in (Okular's Ctrl+=)
                guest.pump(1.5)
                shots["a.pdf-zoomed"] = guest.shot("view-a.pdf-zoomed")
                if (out / "view-a.pdf.ppm").read_bytes() == (out / "view-a.pdf-zoomed.ppm").read_bytes():
                    failures.append("Ctrl+= did not change the PDF's picture")
                guest.key("ctrl", "f")
                guest.type("quokka")
                guest.key("ret")
                guest.pump(1.5)
                shots["a.pdf-found"] = guest.shot("view-a.pdf-found")
                if (out / "view-a.pdf-found.ppm").read_bytes() == (out / "view-a.pdf-zoomed.ppm").read_bytes():
                    failures.append("finding a word did not change the picture")
                guest.key("esc")
            if name == "c.epub":
                guest.key("shift", "dot")  # >: a larger text, laid out again
                guest.pump(2.0)
                shots["c.epub-larger"] = guest.shot("view-c.epub-larger")
                if (out / "view-c.epub.ppm").read_bytes() == (out / "view-c.epub-larger.ppm").read_bytes():
                    failures.append("> did not lay the EPUB out again")
            closed(guest, name)
        guest.key("down")
        guest.key("ret")  # notes.txt: the chooser, view then edit (rc.conf's order)
        guest.pump(1.0)
        shots["chooser"] = guest.shot("chooser")
        guest.key("2")
        guest.key("ret")
        guest.wait_for(br"home: open \S+/notes\.txt \(text/plain\) with program:edit")
        guest.wait_for(br"edit: started with 1 document")
        guest.pump(1.5)
        shots["edit"] = guest.shot("edit")
        guest.key("ctrl", "end")
        guest.type("boot-test-edit-ok")
        guest.key("ctrl", "s")
        guest.wait_for(br"edit: saved \S+/notes\.txt")
        guest.pump(0.5)
        shots["edit-saved"] = guest.shot("edit-saved")
        if (out / "edit.ppm").read_bytes() == (out / "edit-saved.ppm").read_bytes():
            failures.append("typing in edit did not change the picture")
        guest.key("ctrl", "q")
        guest.wait_for(br"home: ended /usr/local/bin/edit \S+/notes\.txt 0")
        guest.pump(0.5)
        guest.key("q")  # out of the file manager, to home's list (drawn, not a new prompt)
        guest.pump(1.0)
        guest.key("down")  # term, the first program
        guest.key("ret")
        guest.wait_for(br"term: started /bin/bash")
        guest.wait_for(br"term: shown 1")
        guest.pump(0.5)
        guest.type("cat /home/*/Documents/notes.txt >/dev/kmsg; ls -n /home/*/Documents/notes.txt >/dev/kmsg")
        guest.key("ret")
        guest.wait_for(br"boot-test-edit-ok")
        guest.wait_for(br" 1000 1000 [0-9]+ .*notes\.txt")
        guest.type("exit")
        guest.key("ret")
        guest.wait_for(br"home: ended /usr/local/bin/term 0")
        guest.wait_for(br"home> ")
        guest.key("end")
        guest.key("ret")
        guest.key("y")
        guest.wait_for(br"init: power off")
        while guest.alive:
            guest.pump(min(0.2, guest.remaining()))
    except (OSError, ValueError, RuntimeError) as error:
        print("FAIL documents boot: " + str(error), file=sys.stderr)
        return 1
    finally:
        if guest is not None:
            guest.close()
    log = (out / "serial.log").read_bytes()
    forbidden = [br"Kernel panic", br"view-render: (refus|cannot)", br"view: Could not open",
                 br"cannot be shown", br"init: cannot run", br"AddressSanitizer"]
    failures += ["unexpected " + repr(p) for p in forbidden if re.search(p, log)]
    names = [n for n, _, _ in DOCUMENTS]
    pictures = {n: (out / ("view-" + n + ".ppm")).read_bytes() for n in names}
    if len(set(pictures.values())) != len(names):
        failures.append("two documents gave the same picture")
    (out / "result.json").write_text(json.dumps({"passed": not failures, "screenshots": shots,
                                                "failures": failures}, indent=2) + "\n")
    if failures:
        print("FAIL documents boot: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("PASS guest: files opens PDF, Markdown, EPUB, DjVu and a comic book in view on the DRM screen; "
          "zoom, find and a larger text change the picture; a text file chosen for edit is changed and "
          "saved as the user; each returns to home")
    return 0


if __name__ == "__main__":
    sys.exit(main())
