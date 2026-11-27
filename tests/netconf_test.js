// NETCONF request builder test: evaluates the generator block of a rendered page
// (yang2html --host-data) on its own, checks the model and writes the generated
// requests for tests/run_netconf_test.sh to validate with yanglint.
//   node tests/netconf_test.js <page.html> <out-dir> <case-set>
// Prints PASS/FAIL lines; writes <out-dir>/manifest.txt: "<yanglint-type> <file> <ok|fail>".
'use strict';
const fs = require('fs'), path = require('path');
const [, , htmlFile, outDir, set] = process.argv;
const html = fs.readFileSync(htmlFile, 'utf8');
const D = JSON.parse(html.match(/<script type="application\/json" id="data">([\s\S]*?)<\/script>/)[1]);
const block = html.match(/\/\/ netconf:begin[\s\S]*?\/\/ netconf:end/)[0];
const G = Object.assign({}, D.groups);
(function reg(s) { if (s.g != null) G[s.g] = { s: s, m: D.module }; (s.c || []).forEach(reg); })(D.root);
const NC = new Function('D', 'G', block + '\nreturn makeNetconf(D, G);')(D, G);

let fails = 0;
const check = (cond, what) => { console.log((cond ? '  PASS ' : '  FAIL ') + what); if (!cond) fails++; };
fs.mkdirSync(outDir, { recursive: true });
const manifest = [];

// chain of {s, o} like the page's tree: "olt/pon-port/ont", "rpc:reboot-olt", "augment:/if:x/if:y/pon"
// (choice and case nodes on the way are included automatically)
function chain(spec) {
  const out = [{ s: { _root: true }, o: D.module }];
  let cur = null, src = D.module, parts;
  if (spec.startsWith('augment:')) {
    const rest = spec.slice(8), i = rest.indexOf('|');
    const target = i < 0 ? rest : rest.slice(0, i);
    cur = D.root.c.find(s => s.k === 'augment' && s.a === target);
    src = cur.f || D.module;
    out.push({ s: cur, o: src });
    parts = i < 0 ? [] : rest.slice(i + 1).split('/');
  } else {
    const p = spec.split('/'), first = p.shift().replace(/^(rpc|notification|grouping):/, '');
    const KINDS = ['container', 'list', 'leaf', 'leaf-list', 'choice', 'anydata', 'anyxml', 'rpc', 'notification', 'grouping'];
    cur = D.root.c.find(s => s.a === first && KINDS.includes(s.k));
    if (!cur) throw new Error('not found: ' + first);
    src = cur.f || D.module;
    out.push({ s: cur, o: src });
    parts = p;
  }
  for (const name of parts) {
    const find = (s, src) => {
      for (const x of NC.children(s, src)) {
        if (x.s.a === name && x.s.k !== 'choice' && x.s.k !== 'case') return [x];
        if (x.s.k === 'choice' || x.s.k === 'case' || x.s.k === 'input') { const r = find(x.s, x.src); if (r) return [x].concat(r); }
      }
      return null;
    };
    const hit = find(cur, src);
    if (!hit) throw new Error('not found: ' + name + ' in ' + spec);
    hit.forEach(x => out.push({ s: x.s, o: x.src }));
    cur = hit[hit.length - 1].s; src = hit[hit.length - 1].src;
  }
  return out;
}

// builds the request; vals by field name ("key:id" for keys)
function build(spec, opt, named) {
  const M = NC.model(chain(spec));
  const vals = {};
  for (const [k, v] of Object.entries(named || {})) {
    const f = k.startsWith('key:') ? M.keyFields.find(f => f.name === k.slice(4)) : M.valueFields.find(f => f.name === k);
    if (!f) throw new Error('no field ' + k + ' for ' + spec);
    vals[f.id] = v;
  }
  return { M, ...NC.xml(M, Object.assign({ mid: 101, ds: 'running', edit: 'merge' }, opt), vals) };
}

// writes the request (and, for edit-config, its config content) for yanglint.
// expect: for the values (-t edit); the envelope (-t nc-rpc) must always be valid
function emit(name, r, expect) {
  const file = path.join(outDir, name + '.xml');
  fs.writeFileSync(file, r.xml + '\n');
  manifest.push('nc-rpc ' + file + ' ok');
  const m = r.xml.match(/<config>\n([\s\S]*)\n\s*<\/config>/);
  if (m) {
    // content alone (yanglint -t edit validates the values); declare the nc prefix on it
    const content = m[1].replace(/^(\s*<[^\s>/]+)/, '$1 xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"');
    const ef = path.join(outDir, name + '.edit.xml');
    fs.writeFileSync(ef, content + '\n');
    manifest.push('edit ' + ef + ' ' + expect);
  }
}
const has = (r, s) => r.xml.includes(s);

if (set === 'acme-olt') {
  // models: which operations, which fields
  let M = NC.model(chain('olt'));
  check(M && M.ops.join() === 'get-config,edit-config,get', 'olt: get-config, edit-config, get');
  check(M.valueFields.map(f => f.name).join() === 'name,mtu', 'olt: value fields name, mtu');
  M = NC.model(chain('olt/pon-port'));
  check(M.keyFields.map(f => f.name).join() === 'id' && M.keyFields[0].T.base === 'uint16' && M.keyFields[0].T.range === '1..4096',
        'pon-port: key id, uint16 {1..4096} through typedef pon-id');
  check(M.valueFields.map(f => f.name).join() === 'if-ref,type,auto-discovery,allowed-serial',
        'pon-port: leaves incl. choice cases, keys and containers excluded');
  check(M.valueFields.find(f => f.name === 'allowed-serial').ctx === 'choice mode / case manual', 'pon-port: choice/case shown');
  check(M.valueFields.find(f => f.name === 'if-ref').T.base === 'leafref', 'pon-port: if:interface-ref resolved to leafref');
  M = NC.model(chain('olt/pon-port/statistics/rx-octets'));
  check(M.ops.join() === 'get', 'config false leaf (from grouping): get only');
  check(M.valueFields[0].T.name.endsWith('counter64') && M.valueFields[0].T.base === 'uint64', 'yang:counter64 resolved to uint64');
  M = NC.model(chain('olt/pon-port/id'));
  check(M.ops.join() === 'get-config,get', 'list key leaf: no edit-config');
  check(NC.model(chain('notification:ont-alarm')) === null, 'notification: no request');
  check(NC.model(chain('grouping:counters')) === null, 'grouping: no request');
  M = NC.model(chain('olt/pon-port/ont'));
  check(M.keyFields.map(f => f.name).join() === 'id,onu-id', 'ont (submodule grouping): keys id, onu-id');
  M = NC.model(chain('olt/pon-port/ont/last-seen'));
  check(M.valueFields[0].T.name === 'yt:date-and-time' && M.valueFields[0].T.patterns.length > 0, 'yt:date-and-time from the submodule prefix');

  // validation messages
  let r = build('olt/pon-port', { op: 'edit-config', edit: 'create' }, { 'key:id': '5000', type: 'foo', 'allowed-serial': ['ABCD0123ABCD', 'bad'] });
  check(r.problems.some(p => /id: range 1\.\.4096/.test(p)), 'problem: key out of range');
  check(r.problems.some(p => /type: one of: gpon, xgspon/.test(p)), 'problem: enum value');
  check(r.problems.some(p => /allowed-serial: pattern/.test(p)), 'problem: pattern');
  r = build('olt', { op: 'edit-config', edit: 'create' }, { mtu: '70000' });
  check(r.problems.some(p => /mtu: uint16 range/.test(p)) && r.problems.some(p => /name is mandatory/.test(p)), 'problem: uint16 bounds, mandatory leaf');
  r = build('olt/pon-port/ont', { op: 'edit-config' }, { 'key:onu-id': '7' });
  check(r.problems.some(p => /key id is required/.test(p)), 'problem: missing parent key for edit-config');

  // requests (validated by yanglint)
  r = build('olt', { op: 'get-config' }); emit('get-config-olt', r, 'ok');
  check(has(r, '<filter type="subtree">') && has(r, '<olt xmlns="urn:acme:olt"/>'), 'get-config olt: selection node');
  r = build('olt/pon-port/mtu'.replace('/pon-port', ''), { op: 'get' }); emit('get-mtu', r, 'ok');
  r = build('olt/pon-port', { op: 'get-config' }, { 'key:id': '3' }); emit('get-config-pon-port-3', r, 'ok');
  check(has(r, '<pon-port>\n') && has(r, '<id>3</id>'), 'get-config pon-port: key as content match');
  r = build('olt/pon-port/statistics/rx-octets', { op: 'get' }); emit('get-rx-octets', r, 'ok');
  check(!has(r, '<id>') && has(r, '<rx-octets/>'), 'get without keys: all entries');
  r = build('olt', { op: 'edit-config' }, { name: 'olt-1', mtu: '1500' }); emit('edit-olt', r, 'ok');
  check(r.problems.length === 0 && has(r, 'nc:operation="merge"') && has(r, 'xmlns:nc='), 'edit-config olt: no problems, operation attribute');
  r = build('olt/mtu', { op: 'edit-config', edit: 'replace' }, { mtu: '9000' }); emit('edit-mtu', r, 'ok');
  check(has(r, '<mtu nc:operation="replace">9000</mtu>'), 'edit-config leaf: operation on the leaf');
  r = build('olt/pon-port', { op: 'edit-config', edit: 'create' }, { 'key:id': '3', type: 'gpon', 'allowed-serial': ['ABCD0123ABCD', 'EFGH4567EF01'] });
  emit('edit-create-pon-port', r, 'ok');
  check(r.problems.length === 0 && r.xml.indexOf('<id>3</id>') < r.xml.indexOf('<type>'), 'create pon-port: key first, leaf-list entries');
  r = build('olt/pon-port', { op: 'edit-config', edit: 'delete' }, { 'key:id': '3', type: 'gpon' }); emit('edit-delete-pon-port', r, 'ok');
  check(has(r, 'nc:operation="delete"') && !has(r, '<type>'), 'delete pon-port: keys only');
  r = build('olt/pon-port/ont', { op: 'edit-config' }, { 'key:id': '1', 'key:onu-id': '7', serial: 'ALCL0001' }); emit('edit-ont', r, 'ok');
  r = build('olt/pon-port/allowed-serial', { op: 'edit-config', edit: 'delete' }, { 'key:id': '2', 'allowed-serial': ['ABCD0123ABCD'] });
  emit('edit-delete-serial', r, 'ok');
  check(has(r, '<allowed-serial nc:operation="delete">ABCD0123ABCD</allowed-serial>'), 'delete one leaf-list entry');
  r = build('olt/mtu', { op: 'edit-config' }, { mtu: '99999' }); emit('edit-bad-mtu', r, 'fail');   // yanglint must reject
  r = build('rpc:reboot-olt', { op: 'rpc' }, { delay: '5' }); emit('rpc-reboot', r, 'ok');
  check(has(r, '<reboot-olt xmlns="urn:acme:olt">') && has(r, '<delay>5</delay>'), 'rpc: input leaves directly in the rpc element');
  r = build('olt/pon-port/reset', { op: 'action' }, { 'key:id': '4', force: 'true' }); emit('action-reset', r, 'ok');
  check(has(r, '<action xmlns="urn:ietf:params:xml:ns:yang:1">') && has(r, '<reset>') && has(r, '<force>true</force>'), 'action: yang:1 wrapper, path, input');
  r = build('augment:/if:interfaces/if:interface|pon', { op: 'edit-config' }, { 'key:name': 'eth0' }); emit('edit-augment-pon', r, 'ok');
  check(has(r, '<interfaces xmlns="urn:ietf:params:xml:ns:yang:ietf-interfaces">') && has(r, '<name>eth0</name>') &&
        has(r, '<pon xmlns="urn:acme:olt" nc:operation="merge"/>'), 'augment: target in ietf-interfaces ns, key name, augment node in acme ns');
  r = build('augment:/if:interfaces/if:interface|pon/rx-octets', { op: 'get' }, {}); emit('get-augment-rx', r, 'ok');
} else if (set === 'acme-types') {
  let M = NC.model(chain('settings'));
  const f = n => M.valueFields.find(x => x.name === n);
  check(M.valueFields.map(x => x.name).join() === 'kind,ratio,enabled,flags,port,tags,code', 'settings: config leaves (state excluded)');
  check(f('kind').choices.join() === 'acme-olt:gpon', 'identityref: identities derived from olt:pon-type');
  check(f('ratio').T.base === 'decimal64' && f('ratio').T.fd === 2, 'decimal64 through typedef');
  let r = build('settings', { op: 'edit-config' }, { kind: 'acme-olt:gpon', ratio: '12.5', enabled: 'true', flags: 'a b', port: 'any', tags: ['x', 'yy'], code: '123' });
  emit('edit-settings', r, 'ok');
  check(r.problems.length === 0, 'settings: valid values accepted');
  check(has(r, '<kind xmlns:olt="urn:acme:olt">olt:gpon</kind>'), 'identityref: prefix declared on the leaf');
  check(has(r, '<enabled/>'), 'empty leaf');
  r = build('settings', { op: 'edit-config' }, { ratio: '100.123', flags: 'c', port: 'x', tags: ['toolongvalue'], code: '12a' });
  ['ratio: at most 2 decimals', 'flags: bits', 'port: not valid', 'tags: length', 'code: pattern'].forEach(p =>
    check(r.problems.some(x => x.startsWith(p)), 'problem: ' + p));
  r = build('settings', { op: 'edit-config' }, { ratio: '100.50' });
  check(r.problems.some(x => x.startsWith('ratio: range')), 'problem: decimal64 range');
  r = build('augment:/if:interfaces/if:interface|vlan', { op: 'edit-config' }, { 'key:name': 'eth1', vlan: '100' });
  emit('edit-augment-vlan', r, 'ok');
  check(has(r, '<vlan xmlns="urn:acme:types" nc:operation="merge">100</vlan>'), 'augmented leaf in its own namespace');
}

fs.writeFileSync(path.join(outDir, 'manifest.txt'), manifest.join('\n') + '\n');
console.log(fails ? 'NETCONF MODEL TEST FAILED (' + fails + ')' : 'NETCONF MODEL TEST PASSED');
process.exit(fails ? 1 : 0);
