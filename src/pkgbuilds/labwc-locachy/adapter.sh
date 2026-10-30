# LOCachy's package-specific labwc overlay. This file is sourced only by
# makepkg as the isolated build user, never by the host's update checker.
# The generated PKGBUILD supplies expected versions and immutable local
# checksums, then sources this adapter. Unsupported recipe shapes fail.

_locachy_recipe_error() {
    printf 'LOCachy labwc adapter: %s\n' "$*" >&2
    return 1
}

unset epoch pkgbase
_locachy_existing_functions=" $(declare -F | awk '{print $3}' | tr '\n' ' ') "
source "$startdir/.locachy-upstream.PKGBUILD" || return

_locachy_package_names=("${pkgname[@]}")
[[ ${#_locachy_package_names[@]} == 1 && ${_locachy_package_names[0]} == labwc ]] || {
    _locachy_recipe_error 'expected one labwc package (split packages require review)'
    return 1
}
[[ ${pkgbase:-labwc} == labwc && $pkgver == "$_locachy_upstream_pkgver" &&
   $pkgrel == "$_locachy_upstream_pkgrel" && ${epoch:-0} == "$_locachy_upstream_epoch" ]] || {
    _locachy_recipe_error 'upstream recipe metadata differs from its locked .SRCINFO'
    return 1
}
if declare -F pkgver >/dev/null; then
    _locachy_recipe_error 'a dynamic pkgver() is not a stable release recipe'
    return 1
fi
for _locachy_function in $(declare -F | awk '{print $3}'); do
    if [[ $_locachy_function == package_* && $_locachy_existing_functions != *" $_locachy_function "* ]]; then
        _locachy_recipe_error 'split package functions require review'
        return 1
    fi
done
for _locachy_function in prepare build check package; do
    if declare -F "$_locachy_function" >/dev/null; then
        _locachy_definition=$(declare -f "$_locachy_function")
        [[ $_locachy_definition == "$_locachy_function ()"* ]] || {
            _locachy_recipe_error 'unexpected bash function declaration'
            return 1
        }
        eval "${_locachy_definition/#$_locachy_function /_locachy_upstream_$_locachy_function }"
    fi
done
declare -F _locachy_upstream_package >/dev/null || {
    _locachy_recipe_error 'upstream package() is missing'
    return 1
}

# Preserve every upstream checksum algorithm, including a new algorithm
# introduced by a later packaging revision. Never add SKIP for our inputs.
_locachy_original_sources=${#source[@]}
source+=("${_locachy_extra_sources[@]}")
_locachy_checksums=0
for _locachy_algorithm in md5 sha1 sha224 sha256 sha384 sha512 b2; do
    _locachy_sumvar=${_locachy_algorithm}sums
    if declare -p "$_locachy_sumvar" >/dev/null 2>&1; then
        # Only the fixed checksum variable names above enter eval; values
        # remain array expansions and are never re-evaluated as shell text.
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

pkgname=labwc-locachy
pkgbase=labwc-locachy
# A local epoch keeps the first tracked 0.20.2-1 above the old bootstrap
# 0.20.2-3; subsequent upstream pkgver/pkgrel retain pacman's ordering.
epoch=$((_locachy_upstream_epoch + 1))
if [[ $_locachy_upstream_epoch == 0 ]]; then
    provides+=("labwc=$pkgver-$pkgrel")
else
    provides+=("labwc=$_locachy_upstream_epoch:$pkgver-$pkgrel")
fi
conflicts+=(labwc)

# Upstream functions still see their own package identity: many use
# $pkgname as their source directory or install-file prefix. The subshell
# confines function-side metadata changes to their normal build invocation.
_locachy_call_upstream() (
    pkgname=labwc
    pkgbase=labwc
    epoch=$_locachy_upstream_epoch
    pkgver=$_locachy_upstream_pkgver
    pkgrel=$_locachy_upstream_pkgrel
    if declare -F "_locachy_upstream_$1" >/dev/null; then
        "_locachy_upstream_$1"
    fi
)

prepare() {
    _locachy_call_upstream prepare || return
    patch --batch --forward --fuzz=0 -d "$srcdir/labwc" -Np1 -i "$srcdir/clipboard-guard.patch" || return
    patch --batch --forward --fuzz=0 -d "$srcdir/labwc" -Np1 -i "$srcdir/inner-edge-hold.patch" || return
    cp -- "$srcdir/locachy-clipboard.c" "$srcdir/labwc/src/locachy-clipboard.c" || return
    cp -- "$srcdir/locachy-clipboard.h" "$srcdir/labwc/include/locachy-clipboard.h"
}
build() { _locachy_call_upstream build; }
check() { _locachy_call_upstream check; }
package() { _locachy_call_upstream package; }
