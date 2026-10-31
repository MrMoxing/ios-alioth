#!/bin/bash
# Assemble the build workspace out of the two source trees under src/, so that a machine with the
# toolchain can build without knowing which tree a file came from.
#
#   ./ubuntu/sync.sh [target]        default target: $HOME/k40
#
# The two trees are the upstream projects as published; the workspace is their union. Where they
# overlap (tools/, prebuilt/, docs/, LICENSE) the union is taken -- checked file by file against
# the working tree on the phone's own machine on 2026-10-31: 135 files in the union, and every one
# of the 130 files in the working tree's tools/ was present and identical.
#
# Nothing is deleted from the target: this adds and overwrites, it does not prune. A target that is
# already a working tree can be updated with it.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../src" && pwd)
dest=${1:-$HOME/k40}

[ -d "$src/hackios-xnu-kernel-main" ] || { echo "sync: no $src/hackios-xnu-kernel-main" >&2; exit 1; }
[ -d "$src/hackios-xnu-userland-main" ] || { echo "sync: no $src/hackios-xnu-userland-main" >&2; exit 1; }

mkdir -p "$dest"
for tree in hackios-xnu-kernel-main hackios-xnu-userland-main; do
	printf 'sync: %s -> %s\n' "$tree" "$dest"
	(cd "$src/$tree" && tar cf - --exclude=.git .) | (cd "$dest" && tar xf -)
done
find "$dest" -name '*.sh' -type f -exec chmod +x {} + 2>/dev/null || true

printf 'sync: done\n'
[ -d "$dest/ios165/rootfs" ] || printf 'sync: note: %s/ios165/rootfs is absent -- run ios165/fetch.sh (about 5.7 GB from Apple) before a userland build\n' "$dest"
printf 'sync: next: %s %s\n' "$here/build.sh" "$dest"
