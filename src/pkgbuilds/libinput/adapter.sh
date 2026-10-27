# Only isolated makepkg sources the locked Arch recipe. Keep its build, tests
# and checksums. library.patch builds without the debug GUI (upstream's
# documented -Ddebug-gui=false, as libinput-tools); prepare() applies
# circular-scroll.patch, libinput's unreleased circular scrolling fixes and
# Panasonic quirks, to the source.
_locachy_recipe_error() {
    printf 'LOCachy libinput adapter: %s\n' "$*" >&2
    return 1
}

unset epoch pkgbase
_locachy_existing_functions=" $(declare -F | awk '{print $3}' | tr '\n' ' ') "
_locachy_library_recipe="$startdir/.locachy-library.PKGBUILD"
rm -f -- "$_locachy_library_recipe"
patch --batch --forward --fuzz=0 -o "$_locachy_library_recipe" \
    "$startdir/.locachy-upstream.PKGBUILD" "$startdir/library.patch" >&2 || return
source "$_locachy_library_recipe" || return

[[ ${pkgname[*]} == 'libinput libinput-tools' && $pkgbase == libinput &&
   $pkgver == "$_locachy_upstream_pkgver" && $pkgrel == "$_locachy_upstream_pkgrel" &&
   ${epoch:-0} == "$_locachy_upstream_epoch" ]] || {
    _locachy_recipe_error 'upstream split metadata differs from the locked .SRCINFO'
    return 1
}
if declare -F pkgver >/dev/null || declare -F package >/dev/null || declare -F prepare >/dev/null; then
    _locachy_recipe_error 'dynamic versions, a prepare() or a different package layout require review'
    return 1
fi
for _locachy_function in $(declare -F | awk '{print $3}'); do
    if [[ $_locachy_function == package_* && $_locachy_existing_functions != *" $_locachy_function "* &&
          $_locachy_function != package_libinput && $_locachy_function != package_libinput-tools ]]; then
        _locachy_recipe_error 'an unexpected split package requires review'
        return 1
    fi
done
for _locachy_function in build check package_libinput; do
    declare -F "$_locachy_function" >/dev/null || {
        _locachy_recipe_error "upstream $_locachy_function() is missing"
        return 1
    }
done

# Retain every source checksum and PGP key. The extra patches have generated
# checksums under the same algorithms as the upstream sources.
_locachy_original_sources=${#source[@]}
source+=("${_locachy_extra_sources[@]}")
_locachy_checksums=0
for _locachy_algorithm in md5 sha1 sha224 sha256 sha384 sha512 b2; do
    _locachy_sumvar=${_locachy_algorithm}sums
    if declare -p "$_locachy_sumvar" >/dev/null 2>&1; then
        eval "_locachy_sumcount=\${#${_locachy_sumvar}[@]}"
        [[ $_locachy_sumcount == "$_locachy_original_sources" ]] || {
            _locachy_recipe_error 'upstream source/checksum count mismatch'
            return 1
        }
        eval "${_locachy_sumvar}+=(\"\${_locachy_extra_${_locachy_sumvar}[@]}\")"
        _locachy_checksums=1
    fi
done
[[ $_locachy_checksums == 1 ]] || {
    _locachy_recipe_error 'upstream source checksums are missing'
    return 1
}

# Emit only the library, under its own name: labwc, the direct programs and
# everything else that depends on libinput resolve to it. The raised epoch
# keeps this build above the repositories' libinput of the same release;
# libinput-tools requires it.
pkgname=(libinput)
epoch=$((_locachy_upstream_epoch + 1))

prepare() {
    patch --batch --forward --fuzz=0 -d "$srcdir/libinput" -Np1 -i "$srcdir/circular-scroll.patch"
}
