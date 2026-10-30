# libpaper lifecycle patch

[中文](BEHAVIOR.zh.md)

This is a small source patch over the locked official Arch `libpaper` recipe,
not a replacement PKGBUILD. It keeps upstream dependencies, sources, checksums,
build and check functions. The local epoch selects the patched library.

The packaged libpaper 2.2.8 implementation violates its `paperdone()` contract
("free any resources allocated by paperinit"): each parsed catalog line leaks
its `getln` buffer; `paperdone` leaves the catalog list and paper names behind.
Repeated Ghostscript/libspectre initialization therefore leaks independently of
LOCachy's renderer. Changing from an allocated HOME-derived configuration path
to a borrowed `XDG_CONFIG_HOME` also retains the old ownership flag and can free
an environment pointer. Upstream v2.3.0 still has the relevant list/line cleanup
omissions; simply updating the version is not a demonstrated fix.

`lifecycle.patch` frees each parsed line after its fields have been copied,
frees empty papersize-file lines, releases the catalog and diagnostic globals,
and resets the path ownership flag on shutdown. It does not alter paper sizes,
default-selection rules or hide sanitizer diagnostics.

`tools/test-libpaper.sh` runs `test-libpaper.c` against the installed candidate
normally and under ASan/UBSan/LeakSanitizer: repeated init/default/done, repeated
done, switching owned HOME to borrowed XDG paths, and fresh catalog contents.
The same test against the unpatched library triggers an invalid free; the fixed
candidate passes without suppression (`~/x230-dev/libpaper-lifecycle-probe/`,
`stock.log` versus `fixed.log`). The normal prepared package transaction must
co-install this candidate with the renderer dependencies before publishing.

The renderer test now checks **every** child exit, including intermediate
restarts, so a sanitizer failure cannot be discarded by `waitpid(..., NULL, ...)`.
A source-level API probe and Ghostscript/libspectre probe establish the library
origin; this does not make a cached-root run full package-transaction acceptance.

References: [libpaper](https://github.com/rrthomas/libpaper),
[`paper.h` lifecycle contract](https://github.com/rrthomas/libpaper/blob/v2.2.8/lib/paper.h),
[official Arch recipe](https://gitlab.archlinux.org/archlinux/packaging/packages/libpaper).
