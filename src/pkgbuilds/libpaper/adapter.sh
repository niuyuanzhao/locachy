# Isolated makepkg sources the locked official recipe, not a copied recipe.
# Preserve its package, dependencies, sources and build/test functions.
_locachy_recipe_error() {
    printf 'LOCachy libpaper adapter: %s\n' "$*" >&2
    return 1
}

unset epoch pkgbase
source "$startdir/.locachy-upstream.PKGBUILD" || return
[[ ${pkgname[*]} == libpaper && ${pkgbase:-libpaper} == libpaper &&
   $pkgver == "$_locachy_upstream_pkgver" && $pkgrel == "$_locachy_upstream_pkgrel" &&
   ${epoch:-0} == "$_locachy_upstream_epoch" ]] || {
    _locachy_recipe_error 'upstream metadata differs from the locked .SRCINFO'
    return 1
}
if declare -F pkgver >/dev/null; then
    _locachy_recipe_error 'a dynamic version requires review'
    return 1
fi
for _locachy_function in prepare build package; do
    declare -F "$_locachy_function" >/dev/null || {
        _locachy_recipe_error "upstream $_locachy_function() is missing"
        return 1
    }
done
_locachy_definition=$(declare -f prepare)
eval "${_locachy_definition/#prepare /_locachy_upstream_prepare }"
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
epoch=$((_locachy_upstream_epoch + 1))
prepare() {
    _locachy_upstream_prepare || return
    patch --batch --forward --fuzz=0 -d "$srcdir/libpaper-$pkgver" -Np1 -i "$srcdir/lifecycle.patch"
}
