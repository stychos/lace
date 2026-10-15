#!/bin/sh
# Lace macOS frontend
# Bundle the daemon's dynamic libraries into the app.
#
# laced links Homebrew's libpq and libmariadb (and what they pull in:
# OpenSSL, zstd, ...), which a user's Mac does not have. This copies every
# library outside the system into Contents/Frameworks, gives each the
# install name @rpath/<file>, rewrites the references in laced and in the
# copied libraries themselves, and adds the Frameworks rpath to laced.
# Signing happens afterwards (install_name_tool breaks a signature).
#
# Usage: scripts/bundle-libs.sh Lace.app
#
# (c) iloveyou, 2025. MIT License.
# https://github.com/stychos/lace

set -eu
app=${1:?app bundle}
bin="$app/Contents/MacOS/laced"
fw="$app/Contents/Frameworks"
[ -x "$bin" ] || { echo "bundle-libs: no laced in $app" >&2; exit 1; }
mkdir -p "$fw"

# The load commands of a Mach-O file, one path per line (its own id excluded)
deps() {
  otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -v "^$(basename "$1")$" || true
}

is_system() {
  case $1 in
    /usr/lib/*|/System/*) return 0 ;;
    *) return 1 ;;
  esac
}

# Where a dependency's file really is: absolute paths as given,
# @loader_path and @rpath relative to the directory the dependent came from
resolve() {
  dep=$1
  from_dir=$2
  case $dep in
    @loader_path/*) echo "$from_dir/${dep#@loader_path/}" ;;
    @rpath/*) echo "$from_dir/${dep#@rpath/}" ;;
    @executable_path/*) echo "$from_dir/${dep#@executable_path/}" ;;
    *) echo "$dep" ;;
  esac
}

# bundle FILE ORIGINAL_DIR: rewrite FILE's dependencies, copying each
# (variables are local: the function recurses into the copied libraries)
bundle() {
  local file from_dir dep name src
  file=$1
  from_dir=$2
  for dep in $(deps "$file"); do
    is_system "$dep" && continue
    name=$(basename "$dep")
    if [ ! -f "$fw/$name" ]; then
      src=$(resolve "$dep" "$from_dir")
      if [ ! -f "$src" ]; then
        echo "bundle-libs: $file needs $dep, not found at $src" >&2
        exit 1
      fi
      cp "$src" "$fw/$name"
      chmod u+w "$fw/$name"
      install_name_tool -id "@rpath/$name" "$fw/$name" 2>/dev/null \
        || { echo "bundle-libs: could not rename $name" >&2; exit 1; }
      bundle "$fw/$name" "$(dirname "$src")"
    fi
    install_name_tool -change "$dep" "@rpath/$name" "$file" 2>/dev/null \
      || { echo "bundle-libs: could not point $file at @rpath/$name" >&2; exit 1; }
  done
}

bundle "$bin" "$(dirname "$bin")"
if ! otool -l "$bin" | grep -q "path @executable_path/../Frameworks "; then
  # Needs header room: laced links with -headerpad_max_install_names
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$bin" 2>/dev/null \
    || { echo "bundle-libs: could not add the Frameworks rpath to laced" >&2; exit 1; }
fi
echo "bundle-libs: $(ls "$fw" | wc -l | tr -d ' ') libraries in Contents/Frameworks"
