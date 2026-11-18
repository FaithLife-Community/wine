#!/usr/bin/env bash
# Run the isolated WPF reader using the draft Wine runner passed on the command line.
set -euo pipefail
if [[ $# != 1 || ! -x $1 ]]; then
    echo "Usage: bash run-wpf-probe.sh ./unpacked-build/bin/wine" >&2
    exit 2
fi
runner=$(realpath "$1")
export WINEPREFIX=${WINEPREFIX:-"$HOME/.wine-oinos-touch-test"}
unset WAYLAND_DISPLAY
demo=$(mktemp -d)
trap 'rm -rf -- "$demo"' EXIT
cd "$demo"

version=8.0.34
microsoft=https://dotnetcli.azureedge.net/dotnet
probe=https://raw.githubusercontent.com/FaithLife-Community/wine/04fe07147a7c819c14a4a8385313c6b3717ad079/tools/touch-probe
curl -fLsS "$microsoft/Runtime/$version/dotnet-runtime-$version-win-x64.zip" -o runtime.zip
curl -fLsS "$microsoft/WindowsDesktop/$version/windowsdesktop-runtime-$version-win-x64.zip" -o desktop.zip
sha512sum --check <<'HASHES'
d86d165a29dcdb71df23dd150a3fc5db6386fa55da5cb18c0c22889a0dab5071a3a2d852572e6cebfecf12b489870ac76243ae35665a746ed5b1011e7c68979d  runtime.zip
e2d06ff86f9bac6a72e0c624364f400f53d0361220d926db59baeffddc8e9483c4f80cf8eaebd5dc836e07a1d72293c7a75ec2adbf90c33957efe7771f5c2558  desktop.zip
HASHES
unzip -qo runtime.zip -d runtime
unzip -qo desktop.zip -d runtime
curl -fLsS "$probe/WpfTouchScroll.exe" -o WpfTouchScroll.exe
curl -fLsS "$probe/WpfTouchScroll.runtimeconfig.json" -o WpfTouchScroll.runtimeconfig.json
sha256sum --check <<'HASHES'
21efb9d484d130fe1e0bc0aea42635bc3decd2c7540661818d072debfdb60c8e  WpfTouchScroll.exe
f18132f23fd1097367392d5deecc939e04d8c07ea02de3f68a65ca0c6c101e9a  WpfTouchScroll.runtimeconfig.json
HASHES

if [[ ! -f "$WINEPREFIX/user.reg" ]]; then
    WINEDLLOVERRIDES='mscoree,mshtml=' "$runner" cmd.exe /c ver
fi
echo "Drag up inside the reading window. The terminal shows touch IDs and vertical scroll offsets."
"$runner" "$demo/runtime/dotnet.exe" exec WpfTouchScroll.exe
