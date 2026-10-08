#!/bin/sh
# Link the SDK files listed in sdk-files.txt into sdk/ (kbuild wants every
# source below M=) and write sdk-objs.txt for Kbuild.
#   link-sdk.sh <path to broadcom-sdk-416L05>
set -e
cd "$(dirname "$0")"
SDK="$1"
mkdir -p sdk
: > sdk-objs.txt
for f in $(cat sdk-files.txt); do
	[ -f "$SDK/$f" ] || { echo "missing $SDK/$f" >&2; exit 1; }
	ln -sf "$SDK/$f" "sdk/$(basename "$f")"
	echo "sdk/$(basename "$f")" >> sdk-objs.txt
done
