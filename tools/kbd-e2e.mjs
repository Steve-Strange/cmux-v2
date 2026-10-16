// Comprehensive real-event keyboard E2E. Drives REAL HID events via
// tools/cmux-key (CGEvent — same path as a physical keyboard) and verifies via
// CDP + process checks. Run against cmux launched with CMUX_DEBUG_PORT=1.
//   node tools/kbd-e2e.mjs [port]
// Drives the real GUI session (cmux is brought frontmost) — don't touch the
// machine while it runs.
import { execFileSync } from 'node:child_process';
import http from 'node:http';

const PORT = process.argv[2] || '9300';
const KEY = new URL('./cmux-key', import.meta.url).pathname;
const get = (u) => new Promise((res, rej) => http.get(u, (s) => { let d=''; s.on('data',c=>d+=c); s.on('end',()=>res(d)); }).on('error', rej));
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const sh = (c) => { try { return execFileSync('bash', ['-c', c], { encoding: 'utf8' }).trim(); } catch { return ''; } };
const PID = sh(`lsof -nP -i :${PORT} | grep LISTEN | awk '{print $2}' | head -1`);
const key = (...a) => { try { return execFileSync(KEY, a, { encoding: 'utf8' }).trim(); } catch (e) { return 'ERR:' + e.message; } };
const act = () => key('activate', PID);
const alive = () => { try { execFileSync('kill', ['-0', PID]); return true; } catch { return false; } };
const targets = async () => JSON.parse(await get(`http://127.0.0.1:${PORT}/json`));

async function cdpEval(urlSub, expr) {
  const t = (await targets()).find(x => x.type === 'page' && (x.url || '').includes(urlSub));
  if (!t) return { error: 'no target ' + urlSub };
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  try { await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('wserr')); setTimeout(() => rej(new Error('to')), 4000); }); }
  catch (e) { return { error: e.message }; }
  let id = 0;
  const send = (m, p) => new Promise(res => { const i = ++id; const h = ev => { const o = JSON.parse(ev.data); if (o.id === i) { ws.removeEventListener('message', h); res(o); } }; ws.addEventListener('message', h); ws.send(JSON.stringify({ id: i, method: m, params: p || {} })); });
  await send('Runtime.enable');
  const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true });
  ws.close();
  if (r.result?.exceptionDetails) return { error: r.result.exceptionDetails.text || 'exception' };
  return { value: r.result?.result?.value };
}

let pass = 0, fail = 0;
const check = (n, ok, d = '') => { ok ? pass++ : fail++; console.log(`${ok ? 'PASS' : 'FAIL'}  ${n}${d ? '  — ' + d : ''}`); };

async function main() {
  if (!PID) { console.log('no cmux on :' + PORT); process.exit(1); }
  console.log('cmux pid', PID, '|', act());
  await sleep(400);

  // 1. Omnibox navigation: Cmd-L focuses omnibox (via monitor), type URL, Enter.
  // Clean end-to-end keyboard test that doesn't depend on web-content focus.
  act(); await sleep(150);
  key('chord', 'cmd', 'l'); await sleep(400);
  key('type', 'example.com'); await sleep(300); key('key', 'return'); await sleep(2500);
  let urls = (await targets()).filter(t => t.type === 'page').map(t => t.url);
  check('omnibox: type URL + Enter navigates', urls.some(u => u.includes('example.com')), urls.map(u=>u.slice(0,28)).join(' | '));

  // 2. Terminal Ctrl-C interrupts a foreground job.
  act(); await sleep(150);
  key('chord', 'cmd', 'opt', 'right'); await sleep(400);  // focus terminal column
  key('type', 'sleep 43'); await sleep(200); key('key', 'return'); await sleep(700);
  const before = sh(`pgrep -f "sleep 43" | head -1`);
  key('chord', 'ctrl', 'c'); await sleep(900);
  const after = sh(`pgrep -f "sleep 43" | head -1`);
  check('terminal: Ctrl-C interrupts job', before !== '' && after === '', `before=${before||'-'} after=${after||'-'}`);
  // cleanup stray sleep
  if (after) sh(`kill ${after} 2>/dev/null`);

  // 3. Web field: real typing + Cmd-A select all. Open a fresh demo column via
  // Cmd-N (the demo page has a known autofocused text input), which also focuses
  // it as the new column.
  act(); await sleep(150); key('chord', 'cmd', 'n'); await sleep(1800);
  let wt = await cdpEval('cmux-input-test', `(()=>{const i=document.querySelector('input[type=text]');if(!i)return 'NOINPUT';i.value='';i.focus();return 'ok'})()`);
  act(); await sleep(200); key('type', 'hello'); await sleep(500);
  let v = await cdpEval('cmux-input-test', `document.querySelector('input[type=text]')?.value`);
  check('web: real typing reaches input', v.value === 'hello', `setup=${wt.value||wt.error} value=${JSON.stringify(v.value)||v.error}`);
  act(); await sleep(150); key('chord', 'cmd', 'a'); await sleep(300);
  let sel = await cdpEval('cmux-input-test', `(()=>{const i=document.querySelector('input[type=text]');return i?i.selectionStart+'-'+i.selectionEnd:'?'})()`);
  check('web: Cmd-A selects all', sel.value === '0-5', `sel=${sel.value||sel.error}`);

  // 4. DevTools toggle (open + close) — last, so a crash doesn't hide the rest.
  const dt0 = (await targets()).filter(t => (t.url||'').startsWith('devtools://')).length;
  act(); await sleep(150); key('chord', 'cmd', 'opt', 'i'); await sleep(1800);
  const dtOpen = (await targets()).filter(t => (t.url||'').startsWith('devtools://')).length;
  check('devtools: Cmd-Opt-I opens', dtOpen > dt0, `${dt0}->${dtOpen}`);
  act(); await sleep(150); key('chord', 'cmd', 'opt', 'i'); await sleep(2000);
  check('devtools: process alive after close (no crash)', alive());

  console.log(`\n${pass} passed, ${fail} failed`);
}
main().catch(e => { console.log('HARNESS ERROR', e.message); process.exit(1); });
