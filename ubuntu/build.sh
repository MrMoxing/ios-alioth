#!/bin/bash
# Build the whole bootable set for the Redmi K40 out of a synced workspace.
#
#   ./ubuntu/build.sh [workspace]         default workspace: $HOME/k40
#   GL=0 ./ubuntu/build.sh                without the OpenGL ES library (window server stays on CPU)
#
# What common/build.sh does, in this order: xnu/apply.sh puts the work on top of Apple's sources,
# xnu-deps/setup.sh builds the toolchain, xnu-build.sh builds the kernel, xnuloader/build.sh builds
# the loader, userland/build.sh builds root.apfs, mkxnuimg.py makes the UEFI image.
# GL=1 is passed through to userland/build.sh, which is what makes libredwood-gl.dylib and installs
# it with libOSMesa.8.dylib and libredwoodair.dylib in /redwood/lib.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=${1:-$HOME/k40}
gl=${GL-1}

[ -d "$tree" ] || { echo "build: no $tree -- run $here/sync.sh first" >&2; exit 1; }
cd "$tree"

for tool in git curl tar xz make cc clang python3; do
	command -v "$tool" >/dev/null || { echo "build: $tool is missing" >&2; exit 1; }
done

if [ ! -d "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" ]; then
	echo "build: no theos/iPhoneOS16.5.sdk -- the kernel will come from prebuilt/ and the source changes will NOT be built" >&2
fi
if [ ! -d ios165/rootfs ]; then
	echo "build: ios165/rootfs is absent -- the userland step stops here; run ios165/fetch.sh first (about 5.7 GB from Apple)" >&2
fi
if [ "$gl" = 1 ] && [ ! -f xnu-deps/build/mesa/out/libOSMesa.8.dylib ]; then
	echo "build: Mesa has not been built yet; the userland step will fetch and build it (needs meson ninja bison flex llvm-ar)" >&2
fi

args=""
[ "$gl" = 1 ] && args="GL=1"
# the kernel's export check fails after linking on purpose; common/build.sh already judges by the file
./common/build.sh alioth $args

printf '\nbuild: out/alioth-boot.img (fastboot boot) and out/run-payload.bin (kernel + root.apfs)\n'
