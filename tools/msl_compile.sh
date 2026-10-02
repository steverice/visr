#!/bin/bash
# msl_compile.sh FOLDER...: compile every .metal file in the folders with the iOS Metal
# compiler (xcrun metal, the Metal toolchain component); prints each failure's first
# errors and a count, and exits nonzero if any failed
set -u
work=$(mktemp -d)
failed=0
total=0
for folder in "$@"; do
	for source in "$folder"/*.metal; do
		[ -e "$source" ] || continue
		total=$((total + 1))
		if ! xcrun --sdk iphoneos metal -std=metal3.0 -c "$source" -o "$work/shader.air" 2> "$work/errors.txt"; then
			failed=$((failed + 1))
			echo "== $source"
			grep --max-count 5 'error:' "$work/errors.txt"
		fi
	done
done
rm -rf "$work"
echo "MSL: $total compiled, $failed failed"
[ "$failed" -eq 0 ] && [ "$total" -gt 0 ]
