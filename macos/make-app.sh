#!/bin/sh
#
# Build J-Pilot.app, a macOS application bundle, and install it.
#
#   macos/make-app.sh [install-folder]
#
# The default install folder is /Applications.  J-Pilot finds its plugins
# and data through the prefix it was built with, so the app works only in
# the folder it was built for; to put it elsewhere, rebuild for that folder.
#
# Needs Homebrew with gtk+3, libgcrypt, sqlite, gettext, intltool, autoconf,
# automake, libtool, pkg-config and imagemagick, and pilot-link 0.15 or
# later.  pilot-link is looked for in $PILOT_PREFIX (default ~/.local) and
# copied into the app; GTK and the other Homebrew libraries are not, so the
# app needs Homebrew on the Mac it runs on.
#
# The app's own files are in J-Pilot.app/Contents:
#   MacOS/J-Pilot        launcher (sets LANG when started from the Finder)
#   MacOS/jpilot         the program
#   MacOS/jpilot-sync    the sync and Handheld Explorer helper; J-Pilot
#                        looks for it next to itself
#   Frameworks/          pilot-link (libpisock)
#   Resources/           plugins, data, documentation, translations,
#                        jpilot-dump and jpilot-merge
# J-Pilot's data stays in ~/.jpilot, as with any other J-Pilot.

set -e

APP_NAME="J-Pilot"
INSTALL_DIR="${1:-/Applications}"
PILOT_PREFIX="${PILOT_PREFIX:-$HOME/.local}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
APP="$INSTALL_DIR/$APP_NAME.app"
PREFIX="$APP/Contents/Resources"

die() {
   echo "make-app.sh: $*" >&2
   exit 1
}

[ "$(uname)" = Darwin ] || die "this builds a macOS application"
command -v brew >/dev/null || die "Homebrew is needed"
command -v magick >/dev/null || die "ImageMagick is needed (brew install imagemagick)"
[ -f "$PILOT_PREFIX/lib/libpisock.9.dylib" ] ||
   die "pilot-link not found in $PILOT_PREFIX (set PILOT_PREFIX)"
[ -w "$INSTALL_DIR" ] || die "cannot write to $INSTALL_DIR"

VERSION=$(sed -n 's/^AC_INIT(\[jpilot\],\[\([^]]*\)\].*/\1/p' "$SRC/configure.ac")
[ -n "$VERSION" ] || die "cannot find the version in configure.ac"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/jpilot-app.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
BUILD="$WORK/src"
STAGE="$WORK/stage"
STAGED_APP="$STAGE$APP"
mkdir -p "$BUILD" "$STAGE"

echo "== Building J-Pilot $VERSION for $APP"
# A clean copy of the source (with any uncommitted changes), so that the
# source folder and its own build are left alone
(cd "$SRC" && git ls-files -z | xargs -0 tar -cf -) | tar -xf - -C "$BUILD"
cd "$BUILD"
export PATH="$(brew --prefix gettext)/bin:$PATH"
export PKG_CONFIG_PATH="$PILOT_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
NOCONFIGURE=1 sh autogen.sh >"$WORK/autogen.log" 2>&1 ||
   die "autogen.sh failed, see $WORK/autogen.log"
./configure --prefix="$PREFIX" --with-pilot-prefix="$PILOT_PREFIX" \
   >"$WORK/configure.log" 2>&1 || { cat "$WORK/configure.log"; die "configure failed"; }
make -j"$(sysctl -n hw.ncpu)" >"$WORK/make.log" 2>&1 ||
   { tail -30 "$WORK/make.log"; die "make failed"; }
make install DESTDIR="$STAGE" >"$WORK/install.log" 2>&1 ||
   { tail -30 "$WORK/install.log"; die "make install failed"; }

echo "== Assembling $APP_NAME.app"
CONTENTS="$STAGED_APP/Contents"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Frameworks"
mv "$CONTENTS/Resources/bin/jpilot" "$CONTENTS/MacOS/jpilot"
mv "$CONTENTS/Resources/bin/jpilot-sync" "$CONTENTS/MacOS/jpilot-sync"

# pilot-link goes inside the app; the programs find it through their rpath
cp "$PILOT_PREFIX/lib/libpisock.9.dylib" "$CONTENTS/Frameworks/"
chmod u+w "$CONTENTS/Frameworks/libpisock.9.dylib"
OLD_PISOCK=$(otool -D "$PILOT_PREFIX/lib/libpisock.9.dylib" | tail -1)
install_name_tool -id @rpath/libpisock.9.dylib "$CONTENTS/Frameworks/libpisock.9.dylib"
for f in "$CONTENTS/MacOS/jpilot" "$CONTENTS/MacOS/jpilot-sync" \
         "$CONTENTS/Resources/bin/"* "$CONTENTS/Resources/lib/jpilot/plugins/"*.so; do
   [ -f "$f" ] || continue
   if otool -L "$f" | grep -q "$OLD_PISOCK"; then
      install_name_tool -change "$OLD_PISOCK" @rpath/libpisock.9.dylib "$f"
      case "$f" in
         */MacOS/*) install_name_tool -add_rpath @executable_path/../Frameworks "$f" ;;
         */Resources/bin/*) install_name_tool -add_rpath @executable_path/../../Frameworks "$f" ;;
         *) install_name_tool -add_rpath @loader_path/../../../../Frameworks "$f" ;;
      esac
   fi
done

# The launcher.  An app started from the Finder has no LANG, and J-Pilot's
# text conversions need one.  Older macOS versions also pass -psn_...,
# which J-Pilot would read as options.
cat >"$CONTENTS/MacOS/$APP_NAME" <<'EOF'
#!/bin/sh
here="$(cd "$(dirname "$0")" && pwd)"
if [ -z "$LANG" ]; then
   lang=$(defaults read -g AppleLocale 2>/dev/null | sed -e 's/@.*//')
   export LANG="${lang:-en_US}.UTF-8"
fi
for arg do
   shift
   case "$arg" in
      -psn_*) ;;
      *) set -- "$@" "$arg" ;;
   esac
done
exec "$here/jpilot" "$@"
EOF
chmod 755 "$CONTENTS/MacOS/$APP_NAME"

# The icon: J-Pilot's own, scaled up without smoothing to keep its pixels
ICONSET="$WORK/$APP_NAME.iconset"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
   magick "$SRC/icons/jpilot-icon3.xpm" -filter point -resize ${size}x${size} \
      "$ICONSET/icon_${size}x${size}.png"
   magick "$SRC/icons/jpilot-icon3.xpm" -filter point -resize $((size * 2))x$((size * 2)) \
      "$ICONSET/icon_${size}x${size}@2x.png"
done
iconutil -c icns -o "$CONTENTS/Resources/$APP_NAME.icns" "$ICONSET"

cat >"$CONTENTS/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
   <key>CFBundleDevelopmentRegion</key>     <string>en</string>
   <key>CFBundleDisplayName</key>           <string>$APP_NAME</string>
   <key>CFBundleExecutable</key>            <string>$APP_NAME</string>
   <key>CFBundleIconFile</key>              <string>$APP_NAME</string>
   <key>CFBundleIdentifier</key>            <string>org.jpilot.J-Pilot</string>
   <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
   <key>CFBundleName</key>                  <string>$APP_NAME</string>
   <key>CFBundlePackageType</key>           <string>APPL</string>
   <key>CFBundleShortVersionString</key>    <string>$VERSION</string>
   <key>CFBundleVersion</key>               <string>$VERSION</string>
   <key>LSApplicationCategoryType</key>     <string>public.app-category.productivity</string>
   <key>LSMinimumSystemVersion</key>        <string>11.0</string>
   <key>NSHighResolutionCapable</key>       <true/>
   <key>NSHumanReadableCopyright</key>      <string>J-Pilot by Judd Montgomery and others, GPL version 2</string>
</dict>
</plist>
EOF

# Changing the libraries' paths invalidated the programs' signatures, and
# Apple Silicon runs nothing unsigned: sign everything again (ad hoc)
find "$CONTENTS" -type f \( -name '*.dylib' -o -name '*.so' -o -perm -u+x \) | while read -r f; do
   if file "$f" | grep -q 'Mach-O'; then
      out=$(codesign --force --sign - "$f" 2>&1) || { echo "$out" >&2; exit 1; }
   fi
done
codesign --force --sign - "$STAGED_APP" 2>/dev/null ||
   echo "make-app.sh: note: could not sign the app bundle itself (it will still run)"

echo "== Installing $APP"
if pgrep -f "$APP/Contents/MacOS/" >/dev/null; then
   die "$APP_NAME is running from $APP; quit it first"
fi
rm -rf "$APP"
mv "$STAGED_APP" "$APP"
echo "Done: open $APP"
