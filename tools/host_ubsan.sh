#!/bin/sh
# Every firmware's host tests built with UBSan (misaligned accesses abort; undefined behaviour reported): the FM-1's CPU
# faults on an unaligned word, which an x86 host runs without a sound (PRISM-1 1.2 crashed at start-up from one).
# Run before a release:  JIELI_TOOLCHAIN=... AC79_SDK=... sh tools/host_ubsan.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC_SAN=$(mktemp)
printf '#!/bin/sh\nexec gcc -fsanitize=alignment,undefined -fno-sanitize=shift-base -fno-sanitize-recover=alignment -g "$@"\n' > "$CC_SAN"
chmod +x "$CC_SAN"
OUT=$(mktemp -d)
bad=0
for d in chain1:chain-1 prism1:prism-1 toybox4:toybox-4; do
    dir=${d%%:*}; stem=${d#*:}
    cd "$ROOT/$dir"
    rm -rf app/build/host
    CC="$CC_SAN" ./build.sh --host >/dev/null
    for t in app/tests/*.txt; do
        r=$(app/build/host/${stem}_host "$t" "$OUT" 2>&1) || true
        f=$(printf '%s\n' "$r" | grep -c '^FAIL' || true)
        u=$(printf '%s\n' "$r" | grep -c 'runtime error' || true)
        echo "$dir $(basename "$t"): $f failed, $u undefined behaviour"
        [ "$f" = 0 ] && [ "$u" = 0 ] || { bad=1; printf '%s\n' "$r" | grep -A3 'runtime error\|^FAIL' | head -12; }
    done
    rm -rf app/build/host                # (the plain host build again on the next ./build.sh --host)
done
rm -rf "$CC_SAN" "$OUT"
exit $bad
