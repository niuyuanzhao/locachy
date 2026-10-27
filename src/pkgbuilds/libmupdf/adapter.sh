# Only isolated makepkg sources the locked Arch recipe. Keep its build and
# checksums; bundled-cmark-gfm.patch builds libmupdf with MuPDF's own
# cmark-gfm, whose autoheaderid extension Markdown documents need.
_locachy_recipe_error() {
    printf 'LOCachy libmupdf adapter: %s\n' "$*" >&2
    return 1
}

unset epoch pkgbase
_locachy_existing_functions=" $(declare -F | awk '{print $3}' | tr '\n' ' ') "
_locachy_cmark_recipe="$startdir/.locachy-cmark.PKGBUILD"
rm -f -- "$_locachy_cmark_recipe"
patch --batch --forward --fuzz=0 -o "$_locachy_cmark_recipe" \
    "$startdir/.locachy-upstream.PKGBUILD" "$startdir/bundled-cmark-gfm.patch" >&2 || return
source "$_locachy_cmark_recipe" || return

[[ ${pkgname[*]} == 'libmupdf mupdf mupdf-gl mupdf-tools python-mupdf' && $pkgbase == mupdf &&
   $pkgver == "$_locachy_upstream_pkgver" && $pkgrel == "$_locachy_upstream_pkgrel" &&
   ${epoch:-0} == "$_locachy_upstream_epoch" ]] || {
    _locachy_recipe_error 'upstream split metadata differs from the locked .SRCINFO'
    return 1
}
if declare -F pkgver >/dev/null || declare -F package >/dev/null; then
    _locachy_recipe_error 'dynamic versions or a different package layout require review'
    return 1
fi
for _locachy_function in $(declare -F | awk '{print $3}'); do
    if [[ $_locachy_function == package_* && $_locachy_existing_functions != *" $_locachy_function "* &&
          $_locachy_function != package_libmupdf && $_locachy_function != package_mupdf &&
          $_locachy_function != package_mupdf-gl && $_locachy_function != package_mupdf-tools &&
          $_locachy_function != package_python-mupdf ]]; then
        _locachy_recipe_error 'an unexpected split package requires review'
        return 1
    fi
done
for _locachy_function in prepare build package_libmupdf; do
    declare -F "$_locachy_function" >/dev/null || {
        _locachy_recipe_error "upstream $_locachy_function() is missing"
        return 1
    }
done

# Retain every source checksum. The extra recipe patch has a generated
# checksum under the same algorithms as the upstream sources.
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

# Emit only the library, under its own name: everything that depends on
# libmupdf still resolves to it. pkgbase stays mupdf, as upstream's
# prepare() and package_libmupdf() use it for their paths. The raised epoch
# keeps this build above the repositories' libmupdf of the same release.
pkgname=(libmupdf)
epoch=$((_locachy_upstream_epoch + 1))
