#!/bin/sh
# test-view-render.sh: view-render's test, run as root in the development
# root with the source at /work: the samples made, then the renderer and its
# test built normally and with ASan/UBSan, and each run.
set -eu
src=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
dir=/tmp/view-test
rm -rf "$dir"
mkdir -p "$dir"
chmod 755 "$dir"
sh "$src/tools/make-view-samples.sh" "$dir/samples"
libs="$(pkg-config --cflags --libs libspectre libheif libjxl libwebp libwebpdemux libarchive ddjvuapi MagickWand) -lmupdf -lm"
status=0
for build in normal sanitize; do
    flags="-O2 -Wall -Wextra -Werror -D_GNU_SOURCE"
    [ "$build" = normal ] || flags="$flags -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    gcc $flags "$src/progs/view-render.c" "$src/progs/view-image.c" -o "$dir/view-render-$build" $libs
    # The test links view-image.c too, for ImageMagick's policy in it.
    gcc $flags -DVIEW_RENDER_TEST_PATH="\"$dir/view-render-$build\"" \
        "$src/progs/view-client.c" "$src/tools/test-view-render.c" "$src/progs/view-image.c" -o "$dir/test-$build" \
        $(pkg-config --cflags --libs MagickWand) -lm
    echo "== $build build"
    "$dir/test-$build" "$dir/samples" "$dir/view-render-$build" || status=1
    # Trim Margins' content box, as Okular's imageBoundingBox finds it.
    gcc $flags -DVIEW_RENDER_TEST_PATH="\"$dir/view-render-$build\"" \
        "$src/progs/view-client.c" "$src/tools/test-view-bbox.c" -o "$dir/test-bbox-$build" -lm
    "$dir/test-bbox-$build" "$dir/bbox-$build" || status=1
done
exit $status
