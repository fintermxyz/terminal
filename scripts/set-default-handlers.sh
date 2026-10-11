#!/bin/sh
# Register terminal.app with LaunchServices and (optionally, via duti)
# make it the DEFAULT handler for the file types it declares.
set -e
APP="$(cd "$(dirname "$0")/.." && pwd)/terminal.app"
BUNDLE_ID="xyz.finterm.terminal"

if [ ! -d "$APP" ]; then
    echo "terminal.app not found — run 'make app' first." >&2
    exit 1
fi

LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"
echo "registering $APP with LaunchServices..."
"$LSREGISTER" -f "$APP"

if ! command -v duti >/dev/null 2>&1; then
    cat <<EOF

'duti' is not installed, so file types were registered but not made default.
Either:
  brew install duti   # then re-run this script
or right-click a file in Finder -> Get Info -> Open with: terminal -> Change All.
EOF
    exit 0
fi

echo "setting terminal.app as default handler via duti..."
# 3D models: own these outright
for uti in public.geometry-definition-format \
           public.standard-tesselated-geometry-format; do
    duti -s "$BUNDLE_ID" "$uti" viewer || true
done
for ext in obj stl; do
    duti -s "$BUNDLE_ID" "$ext" viewer || true
done
# images + text: uncomment to take these over too
# for ext in png jpg jpeg gif bmp tga txt log md json yaml; do
#     duti -s "$BUNDLE_ID" "$ext" viewer || true
# done
echo "done."
