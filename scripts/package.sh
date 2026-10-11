#!/bin/sh
# Build a self-contained terminal.app and package it as dist/terminal.dmg
# (drag-to-Applications) and dist/terminal.pkg (installer into /Applications).
#
# Homebrew dylibs (glfw, freetype, luajit, and what they pull in) are copied
# into Contents/Frameworks and relinked, so the app runs on Macs without them.
set -e
cd "$(dirname "$0")/.."

VERSION=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' assets/Info.plist)
APP=terminal.app
FW=$APP/Contents/Frameworks
DIST=dist

make app
mkdir -p "$FW" "$DIST"

# non-system dylibs a binary links against
deps() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' |
        grep -v -e '^/System/' -e '^/usr/lib/' -e '^@'
}

# copy every dependency (recursively) into Frameworks, then point each binary
# at the bundled copy
queue="$APP/Contents/MacOS/terminal"
while [ -n "$queue" ]; do
    bin=${queue%%
*}
    [ "$bin" = "$queue" ] && queue="" || queue=${queue#*
}
    for dep in $(deps "$bin"); do
        name=$(basename "$dep")
        if [ ! -f "$FW/$name" ]; then
            cp -L "$dep" "$FW/$name"
            chmod u+w "$FW/$name"
            install_name_tool -id "@rpath/$name" "$FW/$name" 2>/dev/null
            queue="$queue${queue:+
}$FW/$name"
        fi
        install_name_tool -change "$dep" "@rpath/$name" "$bin" 2>/dev/null
    done
done
install_name_tool -add_rpath @executable_path/../Frameworks \
    "$APP/Contents/MacOS/terminal" 2>/dev/null || true
for lib in "$FW"/*.dylib; do
    install_name_tool -add_rpath @loader_path "$lib" 2>/dev/null || true
done

# relinking invalidates signatures; re-sign ad hoc (inside out)
for lib in "$FW"/*.dylib; do codesign --force -s - "$lib"; done
codesign --force -s - "$APP"
codesign --verify --deep "$APP"

if deps "$APP/Contents/MacOS/terminal" | grep -q . ; then
    echo "error: app still links outside the bundle:" >&2
    deps "$APP/Contents/MacOS/terminal" >&2
    exit 1
fi

# --- dmg: the app next to an /Applications shortcut
STAGE=$(mktemp -d)
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DIST/terminal.dmg"
hdiutil create -volname "terminal $VERSION" -srcfolder "$STAGE" \
    -fs HFS+ -format UDZO -ov "$DIST/terminal.dmg" >/dev/null
rm -rf "$STAGE"

# --- pkg: installs into /Applications
STAGE=$(mktemp -d)
cp -R "$APP" "$STAGE/"
pkgbuild --root "$STAGE" --install-location /Applications \
    --identifier xyz.finterm.terminal.pkg --version "$VERSION" \
    "$DIST/terminal.pkg" >/dev/null
rm -rf "$STAGE"

echo "built $DIST/terminal.dmg and $DIST/terminal.pkg (v$VERSION)"
shasum -a 256 "$DIST/terminal.dmg"
