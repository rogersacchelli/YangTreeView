#!/usr/bin/env bash
# NETCONF request builder test (Linux/WSL; needs node and yanglint from libyang):
#   tests/run_netconf_test.sh <yang2html binary>
# Renders the sample modules as the Notepad++ panel gets them, runs tests/netconf_test.js
# and validates every generated request with yanglint against the YANG models:
# the whole message (-t nc-rpc, with ietf-netconf) and edit-config content (-t edit).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="$1"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ietf-netconf (defines <edit-config> and the nc:operation attribute) + imports, pinned
models="${YTV_IETF_CACHE:-$work/ietf}"
mkdir -p "$models"
while read -r sha file; do
  if [ ! -f "$models/$file" ]; then
    curl -fsSL -o "$models/$file" "https://raw.githubusercontent.com/YangModels/yang/main/standard/ietf/RFC/$file"
  fi
  echo "$sha  $models/$file" | sha256sum -c --quiet -
done <<'EOF'
6c1d0b39e789b53ea13b7ea6703c2dcb199157a7b011ce30af4ca61b8ba9b13f ietf-netconf@2011-06-01.yang
e03f91317f9538a89296e99df3ff0c4003cdfea70bf517407643b3ec13c1ed25 ietf-netconf-acm@2018-02-14.yang
12d98b0143a5ca5095b36420f9ebc1ffa61cfd2eaa850080244cadf01b86ddf9 ietf-inet-types@2013-07-15.yang
EOF

fail=0
run_set() {  # <case-set> <module file> "<more implemented modules>" <search dirs...>
  local set="$1" mod="$2" more="$3"; shift 3
  local dirs=() pflags=()
  for d in "$@"; do dirs+=(-p "$d"); pflags+=(-p "$d"); done
  echo "== $set"
  if [[ "$bin" == *.exe ]]; then   # Windows build run from WSL: Windows paths
    local wd=() ; for d in "$@"; do wd+=(-p "$(wslpath -w "$d")"); done
    "$bin" "$(wslpath -w "$mod")" "${wd[@]}" --host-data -o "$(wslpath -w "$work/$set.html")" >/dev/null
  else
    "$bin" "$mod" "${dirs[@]}" --host-data -o "$work/$set.html" >/dev/null
  fi
  node "$here/netconf_test.js" "$work/$set.html" "$work/$set" "$set" || fail=1
  while read -r type file expect; do
    [ -n "$type" ] || continue
    if yanglint "${pflags[@]}" -p "$models" -t "$type" "$models/ietf-netconf@2011-06-01.yang" "$mod" $more "$file" >"$work/out.txt" 2>&1; then got=ok; else got=fail; fi
    if [ "$got" = "$expect" ]; then
      echo "  PASS yanglint -t $type $(basename "$file") ($expect)"
    else
      echo "  FAIL yanglint -t $type $(basename "$file"): expected $expect, got $got"; sed 's/^/       /' "$work/out.txt"; cat "$file" | sed 's/^/       | /'; fail=1
    fi
  done < "$work/$set/manifest.txt"
}
run_set acme-olt "$here/sample/acme-olt.yang" "" "$here/sample"
# acme-olt implemented too: identityref values use its identities
run_set acme-types "$here/netconf/acme-types.yang" "$here/sample/acme-olt.yang" "$here/netconf" "$here/sample"
[ $fail = 0 ] && echo "NETCONF TEST PASSED" || { echo "NETCONF TEST FAILED"; exit 1; }
