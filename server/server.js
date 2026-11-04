'use strict';
// ColdChain server: simulated sensors + login/roles (server-enforced) + inventory + public QR status.
const http = require('http'), fs = require('fs'), path = require('path'), crypto = require('crypto');
const QRCode = require('qrcode');
const PORT = process.env.PORT || 3000;
const SECRET = process.env.SECRET || crypto.randomBytes(32).toString('hex');
const DB = path.join(__dirname, 'data.json');
const BUDGET_MAX = 3600;

// ---------- persistence ----------
const hash = (p, s) => crypto.scryptSync(p, s, 32).toString('hex');
function mkUser(u, p, role) { const s = crypto.randomBytes(8).toString('hex'); return { u, s, h: hash(p, s), role }; }
let db;
try { db = JSON.parse(fs.readFileSync(DB)); } catch {
  const d = Date.now(), day = 864e5;
  db = {
    users: [mkUser('admin', 'admin123', 'admin'), mkUser('pharma', 'pharma123', 'pharmacist'), mkUser('inspector', 'inspect123', 'inspector')],
    batches: [
      { id: 'B-1001', vaccine: 'Pfizer COVID-19', qty: 120, fridge: 0, expiry: d + 90 * day, added: d, by: 'admin', heat: 0 },
      { id: 'B-1002', vaccine: 'MMR', qty: 200, fridge: 0, expiry: d + 200 * day, added: d, by: 'admin', heat: 0 },
      { id: 'B-1003', vaccine: 'Hepatitis B', qty: 80, fridge: 1, expiry: d + 20 * day, added: d, by: 'admin', heat: 0 },
    ], log: [], seq: 1004,
  };
}
const save = () => fs.writeFileSync(DB, JSON.stringify(db));
const audit = (u, a) => { db.log.unshift({ t: Date.now(), u, a }); db.log.length = Math.min(db.log.length, 100); save(); };

// ---------- sensor simulation (replaces DS18B20 / reed / relay / ADC) ----------
const fr = [0, 1].map(i => ({
  t: 4.5 + i, target: 5, comp: false, door: false, doorSince: 0, budget: BUDGET_MAX,
  doorUntil: 0, nextDoor: Date.now() + 20000 + Math.random() * 30000,
}));
const sim = { power: 'MAINS', pfUntil: 0, nextPf: Date.now() + 120000 + Math.random() * 120000, batt: 100 };
const rnd = (a, b) => a + Math.random() * (b - a);

function tick() {            // 1 Hz
  const now = Date.now();
  if (sim.power === 'MAINS' && now > sim.nextPf) { sim.power = 'BATTERY'; sim.pfUntil = now + rnd(30, 90) * 1000; }
  if (sim.power === 'BATTERY' && now > sim.pfUntil) { sim.power = 'MAINS'; sim.nextPf = now + rnd(120, 300) * 1000; }
  sim.batt = Math.max(5, Math.min(100, sim.batt + (sim.power === 'BATTERY' ? -0.15 : 0.1)));
  fr.forEach((f, i) => {
    if (!f.door && now > f.nextDoor) { f.door = true; f.doorSince = now; f.doorUntil = now + rnd(8, 45) * 1000; }
    if (f.door && now > f.doorUntil) { f.door = false; f.nextDoor = now + rnd(30, 120) * 1000; }
    // thermostat with hysteresis (5C target, 3..7 band); compressor needs power (battery runs compressors off in sim)
    const canRun = sim.power === 'MAINS' || sim.batt > 30;
    if (f.t > 6.2) f.comp = canRun; else if (f.t < 3.8) f.comp = false;
    if (!canRun) f.comp = false;
    // physics: ambient 28C leak (faster with door open), cooling when compressor on, noise
    const leak = (28 - f.t) * (f.door ? 0.005 : 0.0015);
    const cool = f.comp ? -0.06 : 0;
    f.t += leak + cool + rnd(-0.03, 0.03);
    f.t = Math.max(-2, Math.min(30, f.t));
    if (f.t > 12) f.budget -= 2; else if (f.t > 8) f.budget -= 1;
    else if (f.t >= 2 && f.t <= 8 && f.budget < BUDGET_MAX) f.budget += 0;   // budget is consumed, never refilled
    f.budget = Math.max(0, f.budget);
    // heat exposure for batches in this fridge
    db.batches.forEach(b => { if (b.fridge === i && f.t > 8) b.heat = Math.min(BUDGET_MAX, (b.heat || 0) + (f.t > 12 ? 2 : 1)); });
  });
  if (tick.n++ % 30 === 0) save();
}
tick.n = 0;
setInterval(tick, 1000);

const fridgeStatus = f => f.budget <= 0 ? 'DISCARD' : (f.t >= 2 && f.t <= 8 ? (f.budget < BUDGET_MAX / 5 ? 'WARNING' : 'SAFE') : (f.t > 10 || f.t < 1 ? 'BREACH' : 'WARNING'));
function batchStatus(b) {
  if (Date.now() > b.expiry || b.heat >= BUDGET_MAX) return 'Discard';
  const f = fr[b.fridge];
  if (b.heat >= BUDGET_MAX * 0.8 || (f && f.t > 8)) return 'At Risk';
  return 'Safe';
}

// ---------- auth (signed token in HttpOnly cookie) ----------
const b64 = s => Buffer.from(s).toString('base64url');
const sign = s => crypto.createHmac('sha256', SECRET).update(s).digest('base64url');
function makeToken(u) { const p = b64(JSON.stringify({ u: u.u, role: u.role, exp: Date.now() + 8 * 36e5 })); return p + '.' + sign(p); }
function auth(req) {
  const m = /(?:^|;\s*)cc=([^;]+)/.exec(req.headers.cookie || ''); if (!m) return null;
  const [p, s] = m[1].split('.'); if (!p || !s) return null;
  const a = Buffer.from(sign(p)), b = Buffer.from(s);
  if (a.length !== b.length || !crypto.timingSafeEqual(a, b)) return null;
  try { const d = JSON.parse(Buffer.from(p, 'base64url')); if (d.exp < Date.now()) return null; return db.users.find(x => x.u === d.u) || null; } catch { return null; }
}
// permission matrix, checked on the server for every API call
const PERM = {
  admin:      ['dash', 'inv_view', 'inv_add', 'inv_move', 'inv_delete', 'users', 'audit', 'sim'],
  pharmacist: ['dash', 'inv_view', 'inv_add', 'inv_move'],
  inspector:  ['dash', 'inv_view', 'audit'],
};
const can = (u, p) => u && PERM[u.role].includes(p);
const fails = {};   // simple login rate limit per ip
// ---------- http helpers ----------
const json = (res, c, o, h = {}) => { res.writeHead(c, { 'Content-Type': 'application/json', ...h }); res.end(JSON.stringify(o)); };
const body = req => new Promise(r => { let s = ''; req.on('data', c => { s += c; if (s.length > 1e5) req.destroy(); }); req.on('end', () => { try { r(JSON.parse(s || '{}')); } catch { r({}); } }); });
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };

http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x'), p = url.pathname, me = auth(req);
  try {
    // ---- public (no login): QR batch page + its data + QR image ----
    if (p.startsWith('/api/public/batch/')) {
      const b = db.batches.find(x => x.id === decodeURIComponent(p.split('/').pop()));
      if (!b) return json(res, 404, { error: 'Batch not found' });
      return json(res, 200, { id: b.id, vaccine: b.vaccine, expiry: b.expiry, status: batchStatus(b), expired: Date.now() > b.expiry });   // no fridge/qty/user info
    }
    if (p.startsWith('/qr/')) {
      const id = decodeURIComponent(p.split('/')[2].replace(/\.svg$/, ''));
      const base = process.env.PUBLIC_URL || `http://${req.headers.host}`;
      const svg = await QRCode.toString(`${base}/b/${encodeURIComponent(id)}`, { type: 'svg', margin: 1 });
      res.writeHead(200, { 'Content-Type': 'image/svg+xml' }); return res.end(svg);
    }
    if (p.startsWith('/b/')) { res.writeHead(200, { 'Content-Type': 'text/html' }); return res.end(fs.readFileSync(path.join(__dirname, 'public/batch.html'))); }

    // ---- auth ----
    if (p === '/api/login' && req.method === 'POST') {
      const ip = req.socket.remoteAddress, f = fails[ip] || { n: 0, t: 0 };
      if (f.n >= 5 && Date.now() - f.t < 60000) return json(res, 429, { error: 'Too many attempts, wait 1 minute' });
      const { username, password } = await body(req);
      const u = db.users.find(x => x.u === username);
      const ok = u && crypto.timingSafeEqual(Buffer.from(hash(String(password || ''), u.s)), Buffer.from(u.h));
      if (!ok) { fails[ip] = { n: f.n + 1, t: Date.now() }; return json(res, 401, { error: 'Wrong username or password' }); }
      delete fails[ip]; audit(u.u, 'login');
      return json(res, 200, { ok: true, role: u.role }, { 'Set-Cookie': `cc=${makeToken(u)}; HttpOnly; SameSite=Strict; Path=/; Max-Age=28800` });
    }
    if (p === '/api/logout') return json(res, 200, { ok: true }, { 'Set-Cookie': 'cc=; HttpOnly; Path=/; Max-Age=0' });
    if (p === '/api/me') return me ? json(res, 200, { u: me.u, role: me.role, perms: PERM[me.role] }) : json(res, 401, { error: 'login' });

    // ---- everything below needs login + permission ----
    if (p.startsWith('/api/')) {
      if (!me) return json(res, 401, { error: 'Login required' });
      const need = (perm) => { if (!can(me, perm)) throw { http: 403, msg: `Role "${me.role}" is not allowed (${perm})` }; return true; };

      if (p === '/api/dashboard' && need('dash')) {
        return json(res, 200, {
          power: sim.power, battery: Math.round(sim.batt), up: Math.round(process.uptime()),
          fridges: fr.map(f => ({
            t: +f.t.toFixed(1), status: fridgeStatus(f), door: f.door, door_open_s: f.door ? Math.round((Date.now() - f.doorSince) / 1000) : 0,
            comp: f.comp, budget_pct: Math.round(f.budget * 100 / BUDGET_MAX),
          })),
        });
      }
      if (p === '/api/batches' && req.method === 'GET' && need('inv_view'))
        return json(res, 200, db.batches.map(b => ({ ...b, status: batchStatus(b), heat_pct: Math.round(b.heat * 100 / BUDGET_MAX) })));
      if (p === '/api/batches' && req.method === 'POST' && need('inv_add')) {
        const d = await body(req);
        const qty = parseInt(d.qty), fridge = parseInt(d.fridge), expiry = Date.parse(d.expiry);
        if (!d.vaccine || typeof d.vaccine !== 'string' || d.vaccine.length > 60 || !(qty > 0 && qty < 1e6) || ![0, 1].includes(fridge) || isNaN(expiry))
          return json(res, 400, { error: 'Invalid vaccine / qty / fridge / expiry' });
        const b = { id: 'B-' + db.seq++, vaccine: d.vaccine.trim(), qty, fridge, expiry, added: Date.now(), by: me.u, heat: 0 };
        db.batches.push(b); audit(me.u, `add ${b.id} -> fridge ${fridge + 1}`); return json(res, 200, b);
      }
      let m = /^\/api\/batches\/([^/]+)\/move$/.exec(p);
      if (m && req.method === 'POST' && need('inv_move')) {
        const b = db.batches.find(x => x.id === m[1]), d = await body(req), to = parseInt(d.to);
        if (!b) return json(res, 404, { error: 'Not found' });
        if (![0, 1].includes(to)) return json(res, 400, { error: 'Invalid fridge' });
        if (b.fridge === to) return json(res, 400, { error: 'Already in that fridge' });
        audit(me.u, `move ${b.id} fridge ${b.fridge + 1} -> ${to + 1}`); b.fridge = to; save(); return json(res, 200, b);
      }
      m = /^\/api\/batches\/([^/]+)$/.exec(p);
      if (m && req.method === 'DELETE' && need('inv_delete')) {
        const i = db.batches.findIndex(x => x.id === m[1]); if (i < 0) return json(res, 404, { error: 'Not found' });
        audit(me.u, `delete ${m[1]}`); db.batches.splice(i, 1); save(); return json(res, 200, { ok: true });
      }
      if (p === '/api/audit' && need('audit')) return json(res, 200, db.log);
      if (p === '/api/users' && need('users')) return json(res, 200, db.users.map(u => ({ u: u.u, role: u.role })));
      if (p === '/api/sim' && req.method === 'POST' && need('sim')) {   // admin demo controls
        const d = await body(req);
        if (d.cmd === 'door' && fr[d.i]) { fr[d.i].door = !fr[d.i].door; fr[d.i].doorSince = Date.now(); fr[d.i].doorUntil = Date.now() + 60000; }
        else if (d.cmd === 'power') { sim.power = sim.power === 'MAINS' ? 'BATTERY' : 'MAINS'; sim.pfUntil = Date.now() + 60000; }
        else if (d.cmd === 'reset' && fr[d.i]) { fr[d.i].budget = BUDGET_MAX; db.batches.forEach(b => { if (b.fridge === d.i) b.heat = 0; }); save(); audit(me.u, `reset fridge ${d.i + 1}`); }
        else return json(res, 400, { error: 'bad cmd' });
        return json(res, 200, { ok: true });
      }
      return json(res, 404, { error: 'Unknown endpoint' });
    }

    // ---- static ----
    const f = path.join(__dirname, 'public', p === '/' ? 'index.html' : p);
    if (f.startsWith(path.join(__dirname, 'public')) && fs.existsSync(f) && fs.statSync(f).isFile()) {
      res.writeHead(200, { 'Content-Type': MIME[path.extname(f)] || 'application/octet-stream' }); return res.end(fs.readFileSync(f));
    }
    res.writeHead(404); res.end('Not found');
  } catch (e) {
    if (res.headersSent) return;
    if (e && e.http) return json(res, e.http, { error: e.msg });
    console.error(e); json(res, 500, { error: 'Server error' });
  }
}).listen(PORT, () => console.log(`ColdChain on http://localhost:${PORT}  (admin/admin123, pharma/pharma123, inspector/inspect123)`));
