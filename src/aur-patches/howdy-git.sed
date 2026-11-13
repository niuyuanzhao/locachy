# Howdy 293e3a9 split the root Meson project into howdy/ and howdy-gtk/.
# Keep both outputs of the AUR package, with Arch's original build defaults.
# This is one guarded replacement: if any old setup/build/install line has
# changed, leave the recipe unchanged so locachy-build stops for review.
# Remove this patch once the AUR recipe builds both split projects itself.
:recipe
$!{
N
b recipe
}
s@\nprepare() {\n[[:blank:]]*cd "$srcdir"\n[[:blank:]]*meson setup --reconfigure "$_pkgbase" "$_pkgbase/build"\n}\n\nbuild() {\n[[:blank:]]*cd "$srcdir"\n[[:blank:]]*arch-meson "$_pkgbase" build\n[[:blank:]]*meson compile -C build\n}\n\npackage() {\n[[:blank:]]*cd "$srcdir"\n[[:blank:]]*meson install -C build --destdir "$pkgdir"@\nprepare() {\n\tcd "$srcdir"\n\t: # Both projects are configured in build(), with Arch packaging defaults.\n}\n\nbuild() {\n\tcd "$srcdir"\n\tarch-meson "$_pkgbase/howdy" build\n\tmeson compile -C build\n\tarch-meson "$_pkgbase/howdy-gtk" build-gtk\n\tmeson compile -C build-gtk\n}\n\npackage() {\n\tcd "$srcdir"\n\tmeson install -C build --destdir "$pkgdir"\n\tmeson install -C build-gtk --destdir "$pkgdir"@
