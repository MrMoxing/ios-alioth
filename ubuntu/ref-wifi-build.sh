#!/bin/bash
# The reference implementation's own Wi-Fi stack, built as it ships: Linux 7.0.14's ath11k,
# compiled against FreeBSD 15.1's LinuxKPI through the reference's wifi/lx layer, linked into
# xnu-8796.121.2 by xnu-build.sh. This is the "take their solution whole" experiment of the
# branch wifi-ref-port; docs/WIFI-REFERENCE-PORT.md says why and what it is worth.
#
#   ubuntu/ref-wifi-build.sh [--fresh] [--fetched]
#     --fresh    throw ~/hakios-ref away and lay it out again
#     --fetched  skip the archives (they are 157 MB + 253 MB and already in xnu-deps/build/ath11k)
#
# Environment: REF   the reference tree on this side (default /mnt/c/Users/MrMoxing/hakios-upstream)
#              W     the workspace in WSL            (default ~/hakios-ref)
#              DEPS  built host tools + SDK to hardlink in (default ~/k40/xnu-deps)
#              XNU   a clone of xnu-8796.121.2 with the tag, to feed xnu/apply.sh
#                                                                 (default ~/k40/xnu-src/xnu-8796.121.2)
set -e
REF=${REF:-/mnt/c/Users/MrMoxing/hakios-upstream}
W=${W:-/home/mrmoxing/hakios-ref}
DEPS=${DEPS:-/home/mrmoxing/k40/xnu-deps}
XNU=${XNU:-/home/mrmoxing/k40/xnu-src/xnu-8796.121.2}
B=$W/xnu-deps/build/ath11k
LINUX_SHA=de9999b784d2293f00d39c62d8f92a08ab8a54bc4e80ffd250a0c09cb07a0f98
FREEBSD_SHA=cf5762da53fd52e1eaf0f9ceee9bf58cbe314c821031d0d9ffa76823185a89e1
FRESH=0; FETCHED=0
for a in "$@"; do
	[ "$a" = "--fresh" ] && FRESH=1
	[ "$a" = "--fetched" ] && FETCHED=1
done

if [ $FRESH = 1 ]; then
	rm -rf "$W"
fi
if [ ! -d "$W/xnu" ]; then
	echo "== the tree"
	mkdir -p "$W/build-logs"
	cp -a "$REF/." "$W/"
	rm -rf "$W/xnu-deps"
	cp -al "$DEPS" "$W/xnu-deps"
	# a Windows checkout carries CRLF in every text file, and the kernel build feeds sed scripts out
	# of makedefs: one trailing \r and the build dies with sed's usage message, hundreds of lines in
	grep -rIl $'\r' "$W" --exclude-dir=xnu-src --exclude-dir=xnu-deps --exclude-dir=.git 2>/dev/null |
		xargs -r sed -i 's/\r$//'
fi
cd "$W"

if [ ! -d xnu-src/xnu-8796.121.2/bsd ]; then
	echo "== the kernel sources under the HAKIOS work"
	XNU_URL=file://$XNU bash xnu/apply.sh > build-logs/apply.log 2>&1 || { tail -20 build-logs/apply.log; exit 1; }
	tail -1 build-logs/apply.log | sed 's/^/  /'
fi

if [ ! -f "$B/out/ath11k.o" ]; then
	mkdir -p "$B"
	if [ $FETCHED = 0 ]; then
		echo "== the two archives (aliyun and ustc: the vendors' own servers are 40x slower from here)"
		if [ "$(sha256sum "$B/linux-7.0.14.tar.xz" 2>/dev/null | cut -d' ' -f1)" != "$LINUX_SHA" ]; then
			rm -f "$B/linux-7.0.14.tar.xz"
			curl -sL --fail -o "$B/linux-7.0.14.tar.xz" \
				https://mirrors.aliyun.com/linux-kernel/v7.x/linux-7.0.14.tar.xz
		fi
		if [ "$(sha256sum "$B/freebsd-15.1-src.txz" 2>/dev/null | cut -d' ' -f1)" != "$FREEBSD_SHA" ]; then
			rm -f "$B/freebsd-15.1-src.txz"
			curl -sL --fail -o "$B/freebsd-15.1-src.txz" \
				https://mirrors.ustc.edu.cn/freebsd/releases/amd64/15.1-RELEASE/src.txz
		fi
	fi
	echo "$LINUX_SHA  $B/linux-7.0.14.tar.xz" | sha256sum -c --quiet
	echo "$FREEBSD_SHA  $B/freebsd-15.1-src.txz" | sha256sum -c --quiet
	echo "== ath11k + LinuxKPI"
	bash tools/mk-ath11k.sh > build-logs/ath11k.log 2>&1 || { tail -25 build-logs/ath11k.log; exit 1; }
	ls -l "$B/out/ath11k.o" | sed 's/^/  /'
	[ -f "$B/out/stubs.txt" ] && wc -l "$B/out/stubs.txt" | sed 's/^/  stubs: /'
fi

echo "== the kernel"
bash xnu-build.sh > build-logs/kernel.log 2>&1 || true
M=$(ls -t xnu-src/xnu-8796.121.2/BUILD/obj/*/mach.development.* 2>/dev/null | head -1)
if [ -n "$M" ]; then
	ls -l "$M" | sed 's/^/  /'
	printf '  hakios strings: '; strings "$M" | grep -c hakios || true
else
	echo "  no kernel; tail of the build:"; tail -12 build-logs/kernel.log | sed 's/^/    /'
	exit 1
fi
