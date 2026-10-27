# Only isolated makepkg sources the locked Arch recipe. Keep its build,
# tests and split staging; cli.patch disables only the documented debug GUI.
_locachy_recipe_error() {
    printf 'LOCachy libinput-tools adapter: %s\n' "$*" >&2
    return 1
}

unset epoch pkgbase
_locachy_existing_functions=" $(declare -F | awk '{print $3}' | tr '\n' ' ') "
_locachy_cli_recipe="$startdir/.locachy-cli.PKGBUILD"
rm -f -- "$_locachy_cli_recipe"
patch --batch --forward --fuzz=0 -o "$_locachy_cli_recipe" \
    "$startdir/.locachy-upstream.PKGBUILD" "$startdir/cli.patch" >&2 || return
source "$_locachy_cli_recipe" || return

[[ ${pkgname[*]} == 'libinput libinput-tools' && $pkgbase == libinput &&
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
          $_locachy_function != package_libinput && $_locachy_function != package_libinput-tools ]]; then
        _locachy_recipe_error 'an unexpected split package requires review'
        return 1
    fi
done
for _locachy_function in build check package_libinput package_libinput-tools; do
    declare -F "$_locachy_function" >/dev/null || {
        _locachy_recipe_error "upstream $_locachy_function() is missing"
        return 1
    }
done

# Retain every source checksum and PGP key. The extra recipe patch has a
# generated checksum under the same algorithms as the upstream sources.
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

# makepkg runs package_libinput-tools() even with this one selected output.
# Its first step uses package_libinput() only to stage files, never emitting
# a library archive or changing the installed distribution library.
pkgname=(libinput-tools)
pkgbase=libinput-tools
epoch=$((_locachy_upstream_epoch + 1))
# The library is LOCachy's of the same locked recipe (pkgbuilds/libinput), in
# the same transaction; the contract checks the pair. A versioned dependency on
# it could not be met while building, against the repositories' library.
# --printsrcinfo and makepkg dependency resolution see the tools' dependency
# array, not the source package's separate library-only dependencies.
unset depends
extract_function_variable package_libinput-tools depends 1 depends || {
    _locachy_recipe_error 'upstream tools dependencies are missing'
    return 1
}
