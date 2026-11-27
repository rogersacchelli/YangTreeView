#!/usr/bin/env bash
# NETCONF builder in the browser view (no Notepad++ host):
#   tests/run_browser_test.sh <yang2html binary> <chrome/chromium/msedge binary>
# Renders the sample as a browser page, appends a script that selects a node and fills the
# form, and reads the result back from a headless browser's DOM dump.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bin="$1" browser="$2"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fail=0
check() { if eval "$2"; then echo "  PASS $1"; else echo "  FAIL $1"; fail=1; fi; }
winpath() { if [[ "$1" == *.exe ]]; then wslpath -w "$2"; else echo "$2"; fi; }

mod="$here/sample/acme-olt.yang"
"$bin" "$(winpath "$bin" "$mod")" -o "$(winpath "$bin" "$work/page.html")" >/dev/null
"$bin" "$(winpath "$bin" "$mod")" --no-netconf -o "$(winpath "$bin" "$work/plain.html")" >/dev/null

echo "== page data"
check "default page embeds NETCONF data" 'grep -q "\"nc\":{" "$work/page.html"'
check "default page has no local file paths" '! grep -q "\"paths\":{" "$work/page.html"'
check "--no-netconf leaves NETCONF data out" '! grep -q "\"nc\":{" "$work/plain.html"'

# the test script: errors and results go into <pre id="bt">
python3 - "$work/page.html" "$work/test.html" <<'EOF'
import sys
s = open(sys.argv[1], encoding='utf-8').read()
js = r'''<pre id="bt"></pre><script>
(function(){
  var out = {};
  try {
    var row = [].slice.call(document.querySelectorAll('#tree .row')).filter(function(r){
      var n = r.querySelector('.nm'); return n && n.textContent === 'mtu'; })[0];
    row.click();
    out.section = !!document.getElementById('nc');
    out.newTab = !!document.getElementById('ncNew');
    out.ops = [].map.call(document.querySelectorAll('.nc-ops button'), function(b){ return b.dataset.op; }).join(',');
    document.querySelector('.nc-ops button[data-op="edit-config"]').click();
    var i = document.querySelector('#ncV input[type=text]');
    i.value = '9000'; i.dispatchEvent(new Event('input', {bubbles: true}));
    out.xml = document.getElementById('ncOut').textContent;
    i.value = 'abc'; i.dispatchEvent(new Event('input', {bubbles: true}));
    out.bad = i.classList.contains('bad');
    out.problems = document.getElementById('ncProb').textContent;
    document.getElementById('ncCopy').click();
    out.copyOk = true;
  } catch (e) { out.error = String(e && e.stack || e); }
  document.getElementById('bt').textContent = 'RESULT ' + JSON.stringify(out);
})();
</script>'''
i = s.rindex('</body>') if '</body>' in s else len(s)
open(sys.argv[2], 'w', encoding='utf-8').write(s[:i] + js + s[i:])
EOF

echo "== headless browser"
if [[ "$browser" == *.exe ]]; then url="file:///$(wslpath -m "$work/test.html")"; else url="file://$work/test.html"; fi
"$browser" --headless=new --disable-gpu --no-first-run --no-sandbox --user-data-dir="$(winpath "$browser" "$work/profile")" \
  --virtual-time-budget=3000 --dump-dom "$url" 2>/dev/null | python3 -c '
import sys, json, html, re
m = re.search(r"RESULT (\{.*?\})</pre>", sys.stdin.read(), re.S)
print(json.dumps(json.loads(html.unescape(m.group(1)))) if m else "{\"error\": \"no result in DOM dump\"}")' >"$work/result.json"
r() { python3 -c 'import json,sys; v=json.load(open(sys.argv[1])).get(sys.argv[2]); print(v if v is not None else "")' "$work/result.json" "$1"; }
check "page script ran without errors" '[ -z "$(r error)" ] || { r error; false; }'
check "NETCONF section shown in the browser" '[ "$(r section)" = True ]'
check "no Open in new tab button without Notepad++" '[ "$(r newTab)" = False ]'
check "operations for a config leaf" '[ "$(r ops)" = "get-config,edit-config,get" ]'
check "edit-config request with mtu 9000" 'r xml | grep -q "<edit-config>" && r xml | grep -q "<running/>" && r xml | grep -q "<mtu nc:operation=\"merge\">9000</mtu>"'
check "invalid value flagged" '[ "$(r bad)" = True ] && [ -n "$(r problems)" ]'
check "Copy works without the host" '[ "$(r copyOk)" = True ]'
[ $fail = 0 ] && echo "BROWSER TEST PASSED" || { echo "BROWSER TEST FAILED"; cat "$work/result.json"; exit 1; }
