# libpaper lifecycle patch

[中文](BEHAVIOR.zh.md)

The `libpaper` package applies `lifecycle.patch` over the official Arch `libpaper` recipe.

In libpaper 2.2.8, `paperdone()` leaks each parsed catalog line buffer and the catalog list, and retains the `HOME`-derived path ownership flag when switching to `XDG_CONFIG_HOME`. `lifecycle.patch` frees parsed line buffers after copying fields, frees empty lines, releases catalog and diagnostic globals on `paperdone()`, and resets the path ownership flag.

## Tests

```sh
src/tools/test-libpaper.sh
```

References: [libpaper](https://github.com/rrthomas/libpaper), [`paper.h`](https://github.com/rrthomas/libpaper/blob/v2.2.8/lib/paper.h), [Arch recipe](https://gitlab.archlinux.org/archlinux/packaging/packages/libpaper).
