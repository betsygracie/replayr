// Replayr account server: invite-only accounts (username + password, no emails), clip sharing.
// Runs on Cloudflare Workers + D1, both free with no card. Clip uploads need R2 (free tier, but
// Cloudflare asks for a card to turn it on); without the CLIPS binding, uploads are simply off.
//
// Tokens: an access token (1 hour) and a refresh token (60 days, rotated on every use, stored only
// as a hash). Passwords are stored as PBKDF2-SHA256 hashes with a per-user salt.

const ACCESS_TTL = 3600;
const REFRESH_TTL = 60 * 86400;

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

const now = () => Math.floor(Date.now() / 1000);
const enc = new TextEncoder();

function b64url(bytes) {
  let s = '';
  for (const b of new Uint8Array(bytes)) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function randomToken(bytes = 32) {
  return b64url(crypto.getRandomValues(new Uint8Array(bytes)));
}

function shortId() {
  // 10 characters from an unambiguous alphabet, for share links.
  const alphabet = 'abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  const r = crypto.getRandomValues(new Uint8Array(10));
  return Array.from(r, (b) => alphabet[b % alphabet.length]).join('');
}

async function sha256(text) {
  return b64url(await crypto.subtle.digest('SHA-256', enc.encode(text)));
}

async function hmacKey(secret) {
  return crypto.subtle.importKey('raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign', 'verify']);
}

async function signJwt(payload, secret) {
  const head = b64url(enc.encode(JSON.stringify({ alg: 'HS256', typ: 'JWT' })));
  const body = b64url(enc.encode(JSON.stringify(payload)));
  const sig = await crypto.subtle.sign('HMAC', await hmacKey(secret), enc.encode(`${head}.${body}`));
  return `${head}.${body}.${b64url(sig)}`;
}

async function verifyJwt(token, secret) {
  const parts = (token || '').split('.');
  if (parts.length !== 3) return null;
  const sig = Uint8Array.from(atob(parts[2].replace(/-/g, '+').replace(/_/g, '/')), (c) => c.charCodeAt(0));
  const ok = await crypto.subtle.verify('HMAC', await hmacKey(secret), sig, enc.encode(`${parts[0]}.${parts[1]}`));
  if (!ok) return null;
  try {
    const payload = JSON.parse(atob(parts[1].replace(/-/g, '+').replace(/_/g, '/')));
    if (!payload.exp || payload.exp < now()) return null;
    return payload;
  } catch {
    return null;
  }
}

const json = (data, status = 200, headers = {}) =>
  new Response(JSON.stringify(data), { status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store', ...headers } });
const fail = (status, message) => json({ error: message }, status);

function html(body, status = 200) {
  return new Response(body, { status, headers: { 'content-type': 'text/html; charset=utf-8' } });
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

// Best-effort rate limit per isolate (add a Cloudflare rate-limiting rule for a hard limit).
const hits = new Map();
function limited(request, key, max, windowSec) {
  const ip = request.headers.get('cf-connecting-ip') || 'unknown';
  const k = `${key}:${ip}`;
  const t = now();
  const e = hits.get(k) || { start: t, n: 0 };
  if (t - e.start > windowSec) {
    e.start = t;
    e.n = 0;
  }
  e.n++;
  hits.set(k, e);
  if (hits.size > 5000) hits.clear();
  return e.n > max;
}

async function authUser(request, env) {
  const m = /^Bearer (.+)$/.exec(request.headers.get('authorization') || '');
  if (!m) return null;
  const p = await verifyJwt(m[1], env.TOKEN_SECRET);
  if (!p) return null;
  const user = await env.DB.prepare('SELECT * FROM users WHERE id = ?').bind(p.sub).first();
  if (!user || user.banned) return null;
  return user;
}

function publicUser(u, env) {
  return { id: u.id, name: u.display_name, avatar: null, provider: 'replayr', storage: !!(env && env.CLIPS), usedBytes: u.used_bytes, quotaBytes: u.quota_bytes };
}

function publicClip(c, origin) {
  return {
    id: c.id, title: c.title, privacy: c.privacy, sizeBytes: c.size_bytes, duration: c.duration, views: c.views,
    createdAt: c.created_at, status: c.status, url: `${origin}/c/${c.id}`,
  };
}

// ---------------------------------------------------------------------------------------------
// Sign-in
// ---------------------------------------------------------------------------------------------

// Accounts are a username + password, and making one needs an invite code that only the owner can
// generate (at /admin, with the ADMIN_KEY secret). No emails, no third-party sign-in.
const NAME_RE = /^[A-Za-z0-9_.-]{3,24}$/;
const PBKDF2_ITERATIONS = 100000;

async function hashPassword(password, saltB64) {
  const salt = saltB64 ? Uint8Array.from(atob(saltB64), (c) => c.charCodeAt(0)) : crypto.getRandomValues(new Uint8Array(16));
  const key = await crypto.subtle.importKey('raw', enc.encode(password), 'PBKDF2', false, ['deriveBits']);
  const bits = await crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations: PBKDF2_ITERATIONS }, key, 256);
  return { hash: b64url(bits), salt: btoa(String.fromCharCode(...salt)) };
}

function sameString(a, b) {   // constant-time compare
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

function normCode(c) {
  return String(c || '').toUpperCase().replace(/[^A-Z0-9]/g, '');
}

async function register(request, env) {
  if (limited(request, 'register', 10, 3600)) return fail(429, 'Too many attempts. Try again later.');
  const b = await request.json().catch(() => ({}));
  const name = String(b.username || '').trim(), password = String(b.password || ''), code = normCode(b.code);
  if (!NAME_RE.test(name)) return fail(400, 'Usernames are 3-24 letters, numbers, dots, dashes or underscores.');
  if (password.length < 8 || password.length > 200) return fail(400, 'Use a password of at least 8 characters.');
  if (!code) return fail(400, 'You need an invite code to make an account.');
  const invite = await env.DB.prepare('SELECT * FROM invite_codes WHERE code = ?').bind(code).first();
  if (!invite || invite.used_by) return fail(400, "That invite code isn't valid or has already been used.");
  if (await env.DB.prepare('SELECT 1 FROM users WHERE username = ? COLLATE NOCASE').bind(name).first())
    return fail(409, 'That username is taken.');
  const { hash, salt } = await hashPassword(password);
  const id = randomToken(12), t = now();
  // Claim the code first: if two people race for it, only one UPDATE succeeds.
  const claim = await env.DB.prepare('UPDATE invite_codes SET used_by = ?, used_at = ? WHERE code = ? AND used_by IS NULL').bind(id, t, code).run();
  if (!claim.meta || claim.meta.changes !== 1) return fail(400, "That invite code isn't valid or has already been used.");
  await env.DB.prepare('INSERT INTO users (id, username, pass_hash, pass_salt, display_name, created_at) VALUES (?, ?, ?, ?, ?, ?)')
    .bind(id, name, hash, salt, name, t).run();
  const user = await env.DB.prepare('SELECT * FROM users WHERE id = ?').bind(id).first();
  return json(await issueTokens(env, user, b.device));
}

async function login(request, env) {
  if (limited(request, 'login', 20, 600)) return fail(429, 'Too many attempts. Wait a few minutes.');
  const b = await request.json().catch(() => ({}));
  const name = String(b.username || '').trim(), password = String(b.password || '');
  const user = name ? await env.DB.prepare('SELECT * FROM users WHERE username = ? COLLATE NOCASE').bind(name).first() : null;
  // Hash even when the user doesn't exist, so timing doesn't reveal which usernames are taken.
  const { hash } = await hashPassword(password, user ? user.pass_salt : 'AAAAAAAAAAAAAAAAAAAAAA==');
  if (!user || !sameString(hash, user.pass_hash)) return fail(401, 'Wrong username or password.');
  if (user.banned) return fail(403, 'This account has been disabled.');
  return json(await issueTokens(env, user, b.device));
}

async function changePassword(request, env, user) {
  const b = await request.json().catch(() => ({}));
  const { hash } = await hashPassword(String(b.oldPassword || ''), user.pass_salt);
  if (!sameString(hash, user.pass_hash)) return fail(401, 'Your current password is wrong.');
  const np = String(b.newPassword || '');
  if (np.length < 8 || np.length > 200) return fail(400, 'Use a password of at least 8 characters.');
  const h = await hashPassword(np);
  await env.DB.prepare('UPDATE users SET pass_hash = ?, pass_salt = ? WHERE id = ?').bind(h.hash, h.salt, user.id).run();
  await env.DB.prepare('DELETE FROM sessions WHERE user_id = ?').bind(user.id).run();
  return json(await issueTokens(env, { ...user }, b.device));
}

// ---------------------------------------------------------------------------------------------
// Invite codes (owner only)
// ---------------------------------------------------------------------------------------------

function isAdmin(request, env) {
  const key = request.headers.get('x-admin-key') || '';
  return !!env.ADMIN_KEY && env.ADMIN_KEY.length >= 16 && sameString(key, env.ADMIN_KEY);
}

function newCode() {
  const alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
  const r = crypto.getRandomValues(new Uint8Array(12));
  const c = Array.from(r, (b) => alphabet[b % alphabet.length]).join('');
  return `${c.slice(0, 4)}-${c.slice(4, 8)}-${c.slice(8, 12)}`;
}

async function adminApi(request, env, path, method) {
  if (limited(request, 'admin', 30, 600)) return fail(429, 'Too many attempts.');
  if (!isAdmin(request, env)) return fail(401, 'Wrong admin key.');
  if (method === 'POST' && path === '/admin/codes') {
    const b = await request.json().catch(() => ({}));
    const n = Math.max(1, Math.min(50, Number(b.count) || 1)), note = String(b.note || '').slice(0, 80);
    const codes = [];
    for (let i = 0; i < n; i++) {
      const c = newCode();
      await env.DB.prepare('INSERT INTO invite_codes (code, note, created_at) VALUES (?, ?, ?)').bind(normCode(c), note, now()).run();
      codes.push(c);
    }
    return json({ codes });
  }
  if (method === 'GET' && path === '/admin/codes') {
    const r = await env.DB.prepare(`SELECT c.code, c.note, c.created_at, c.used_at, u.username AS used_by
      FROM invite_codes c LEFT JOIN users u ON u.id = c.used_by ORDER BY c.created_at DESC LIMIT 200`).all();
    return json({ codes: r.results.map((c) => ({ ...c, code: `${c.code.slice(0, 4)}-${c.code.slice(4, 8)}-${c.code.slice(8)}` })) });
  }
  if (method === 'DELETE' && path.startsWith('/admin/codes/')) {
    await env.DB.prepare('DELETE FROM invite_codes WHERE code = ? AND used_by IS NULL').bind(normCode(path.slice(13))).run();
    return json({ ok: true });
  }
  return fail(404, 'Not found.');
}

function adminPage() {
  return `<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex"><title>Replayr invite codes</title><style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;font-family:system-ui,sans-serif;color:#fff;background:radial-gradient(circle at 20% 15%,#6a2be0 0,transparent 45%),radial-gradient(circle at 85% 80%,#c42a8e 0,transparent 40%),#0b0614;display:flex;justify-content:center;padding:48px 16px}
.card{width:100%;max-width:560px;padding:28px;border-radius:20px;background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.18)}
h1{margin:0 0 4px;font-size:22px}p{color:rgba(255,255,255,.65);margin:0 0 18px}
input,button{font:inherit;border-radius:10px;border:1px solid rgba(255,255,255,.2);padding:10px 12px;color:#fff;background:rgba(0,0,0,.25)}
button{background:linear-gradient(90deg,#7b2ff7,#e2348f);border:0;cursor:pointer;font-weight:600}button.ghost{background:rgba(255,255,255,.1)}
.row{display:flex;gap:8px;margin-bottom:10px}.row input{flex:1}
.code{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;border-radius:10px;background:rgba(0,0,0,.22);margin-top:6px;font-family:ui-monospace,monospace}
.used{opacity:.45}.small{font-family:system-ui;font-size:12px;color:rgba(255,255,255,.6)}#err{color:#ff8fb3;min-height:20px}
</style></head><body><div class="card"><h1>Invite codes</h1><p>Only you can make these. Each code makes one account.</p>
<div class="row"><input id="key" type="password" placeholder="Admin key" autocomplete="current-password"></div>
<div class="row"><input id="note" placeholder="Who is it for? (optional)"><input id="count" type="number" min="1" max="50" value="1" style="max-width:80px"><button id="gen">Generate</button></div>
<div id="err"></div><div id="list"></div></div><script>
const $=(i)=>document.getElementById(i);$('key').value=sessionStorage.getItem('k')||'';
const api=(m,p,b)=>fetch(p,{method:m,headers:{'x-admin-key':$('key').value,'content-type':'application/json'},body:b?JSON.stringify(b):undefined}).then(async r=>{const j=await r.json();if(!r.ok)throw new Error(j.error);return j});
const esc=(s)=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'})[c]);
async function load(){$('err').textContent='';sessionStorage.setItem('k',$('key').value);try{const j=await api('GET','/admin/codes');
$('list').innerHTML=j.codes.map(c=>'<div class="code'+(c.used_by?' used':'')+'"><span>'+esc(c.code)+' <span class="small">'+esc(c.note)+(c.used_by?' &middot; used by '+esc(c.used_by):'')+'</span></span>'+
(c.used_by?'':'<span><button class="ghost" onclick="navigator.clipboard.writeText(\''+c.code+'\')">Copy</button> <button class="ghost" onclick="del(\''+c.code+'\')">Delete</button></span>')+'</div>').join('')||'<p>No codes yet.</p>';}catch(e){$('err').textContent=e.message}}
async function del(c){await api('DELETE','/admin/codes/'+c).catch(e=>$('err').textContent=e.message);load()}
$('gen').onclick=async()=>{try{await api('POST','/admin/codes',{count:$('count').value,note:$('note').value});$('note').value='';load()}catch(e){$('err').textContent=e.message}};
$('key').onchange=load;if($('key').value)load();
</script></body></html>`;
}

async function issueTokens(env, user, device) {
  const refresh = randomToken(32);
  const t = now();
  await env.DB.prepare('INSERT INTO sessions (refresh_hash, user_id, device, created_at, last_used_at, expires_at) VALUES (?, ?, ?, ?, ?, ?)')
    .bind(await sha256(refresh), user.id, (device || 'PC').slice(0, 64), t, t, t + REFRESH_TTL).run();
  const access = await signJwt({ sub: user.id, exp: t + ACCESS_TTL }, env.TOKEN_SECRET);
  return { accessToken: access, refreshToken: refresh, expiresIn: ACCESS_TTL, user: publicUser(user, env) };
}

async function refreshTokens(request, env) {
  if (limited(request, 'refresh', 60, 600)) return fail(429, 'Too many attempts.');
  const body = await request.json().catch(() => ({}));
  if (typeof body.refreshToken !== 'string') return fail(400, 'Bad request.');
  const hash = await sha256(body.refreshToken);
  const s = await env.DB.prepare('SELECT * FROM sessions WHERE refresh_hash = ?').bind(hash).first();
  if (!s || s.expires_at < now()) return fail(401, 'Signed out. Sign in again.');
  await env.DB.prepare('DELETE FROM sessions WHERE refresh_hash = ?').bind(hash).run();   // rotate
  const user = await env.DB.prepare('SELECT * FROM users WHERE id = ?').bind(s.user_id).first();
  if (!user || user.banned) return fail(401, 'Signed out.');
  return json(await issueTokens(env, user, s.device));
}

async function logout(request, env) {
  const body = await request.json().catch(() => ({}));
  if (typeof body.refreshToken === 'string')
    await env.DB.prepare('DELETE FROM sessions WHERE refresh_hash = ?').bind(await sha256(body.refreshToken)).run();
  return json({ ok: true });
}

// ---------------------------------------------------------------------------------------------
// Clips
// ---------------------------------------------------------------------------------------------

async function createClip(request, env, user) {
  if (limited(request, 'create', 60, 3600)) return fail(429, 'Too many uploads. Try again later.');
  const b = await request.json().catch(() => ({}));
  const size = Number(b.sizeBytes) | 0;
  const maxBytes = parseInt(env.MAX_UPLOAD_MB || '95', 10) * 1024 * 1024;
  if (!(size > 0)) return fail(400, 'Bad request.');
  if (size > maxBytes) return fail(413, `Clips can be up to ${env.MAX_UPLOAD_MB || 95} MB. Export a shorter or smaller version first.`);
  if (user.used_bytes + size > user.quota_bytes) return fail(413, 'Your cloud storage is full. Delete an upload to make room.');
  const privacy = ['public', 'unlisted', 'private'].includes(b.privacy) ? b.privacy : 'unlisted';
  const title = String(b.title || 'Clip').slice(0, 100);
  const id = shortId();
  await env.DB.prepare('INSERT INTO clips (id, user_id, title, privacy, size_bytes, duration, status, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)')
    .bind(id, user.id, title, privacy, size, Number(b.duration) || 0, 'uploading', now()).run();
  return json({ id, uploadPath: `/clips/${id}/upload` });
}

async function uploadClip(request, env, user, id, origin) {
  const clip = await env.DB.prepare('SELECT * FROM clips WHERE id = ? AND user_id = ?').bind(id, user.id).first();
  if (!clip || clip.status !== 'uploading') return fail(404, 'Upload not found.');
  const length = parseInt(request.headers.get('content-length') || '0', 10);
  if (length !== clip.size_bytes || !request.body) return fail(400, 'The file size changed during upload.');
  const key = `clips/${id}.mp4`;
  await env.CLIPS.put(key, request.body, { httpMetadata: { contentType: 'video/mp4' } });
  // Only MP4s: check the "ftyp" box at the start of the file.
  const head = await env.CLIPS.get(key, { range: { offset: 0, length: 12 } });
  const bytes = head ? new Uint8Array(await head.arrayBuffer()) : new Uint8Array();
  if (bytes.length < 8 || String.fromCharCode(...bytes.slice(4, 8)) !== 'ftyp') {
    await env.CLIPS.delete(key);
    await env.DB.prepare('DELETE FROM clips WHERE id = ?').bind(id).run();
    return fail(415, 'Only MP4 videos can be uploaded.');
  }
  await env.DB.batch([
    env.DB.prepare("UPDATE clips SET status = 'ready' WHERE id = ?").bind(id),
    env.DB.prepare('UPDATE users SET used_bytes = used_bytes + ? WHERE id = ?').bind(clip.size_bytes, user.id),
  ]);
  return json(publicClip({ ...clip, status: 'ready' }, origin));
}

async function myClips(env, user, origin) {
  const rows = await env.DB.prepare("SELECT * FROM clips WHERE user_id = ? AND status = 'ready' ORDER BY created_at DESC LIMIT 500").bind(user.id).all();
  return json({ clips: rows.results.map((c) => publicClip(c, origin)), user: publicUser(user, env) });
}

async function updateClip(request, env, user, id, origin) {
  const b = await request.json().catch(() => ({}));
  const clip = await env.DB.prepare('SELECT * FROM clips WHERE id = ? AND user_id = ?').bind(id, user.id).first();
  if (!clip) return fail(404, 'Clip not found.');
  const privacy = ['public', 'unlisted', 'private'].includes(b.privacy) ? b.privacy : clip.privacy;
  const title = typeof b.title === 'string' ? b.title.slice(0, 100) : clip.title;
  await env.DB.prepare('UPDATE clips SET privacy = ?, title = ? WHERE id = ?').bind(privacy, title, id).run();
  return json(publicClip({ ...clip, privacy, title }, origin));
}

async function deleteClip(env, user, id) {
  const clip = await env.DB.prepare('SELECT * FROM clips WHERE id = ? AND user_id = ?').bind(id, user.id).first();
  if (!clip) return fail(404, 'Clip not found.');
  if (env.CLIPS) await env.CLIPS.delete(`clips/${id}.mp4`);
  const freed = clip.status === 'ready' ? clip.size_bytes : 0;
  await env.DB.batch([
    env.DB.prepare('DELETE FROM clips WHERE id = ?').bind(id),
    env.DB.prepare('UPDATE users SET used_bytes = MAX(0, used_bytes - ?) WHERE id = ?').bind(freed, user.id),
  ]);
  return json({ ok: true });
}

// ---------------------------------------------------------------------------------------------
// Share pages (what people open from a copied link) and video streaming
// ---------------------------------------------------------------------------------------------

async function viewableClip(env, id) {
  const clip = await env.DB.prepare(
    "SELECT clips.*, users.display_name AS owner, users.banned FROM clips JOIN users ON users.id = clips.user_id WHERE clips.id = ? AND clips.status = 'ready'"
  ).bind(id).first();
  if (!clip || clip.privacy === 'private' || clip.banned) return null;
  return clip;
}

async function sharePage(env, id, origin, ctx) {
  const clip = await viewableClip(env, id);
  if (!clip) return html(page('Clip not found', 'It may have been deleted or made private.'), 404);
  ctx.waitUntil(env.DB.prepare('UPDATE clips SET views = views + 1 WHERE id = ?').bind(id).run());
  const title = escapeHtml(clip.title), owner = escapeHtml(clip.owner), app = escapeHtml(env.APP_NAME || 'Replayr');
  const video = `${origin}/c/${id}.mp4`;
  return html(`<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>${title} · ${app}</title>
<meta property="og:site_name" content="${app}">
<meta property="og:title" content="${title}">
<meta property="og:description" content="Clipped by ${owner}">
<meta property="og:type" content="video.other">
<meta property="og:url" content="${origin}/c/${id}">
<meta property="og:video" content="${video}">
<meta property="og:video:secure_url" content="${video}">
<meta property="og:video:type" content="video/mp4">
<meta property="og:video:width" content="1280"><meta property="og:video:height" content="720">
<meta name="twitter:card" content="player"><meta name="twitter:player" content="${origin}/c/${id}">
<meta name="twitter:player:width" content="1280"><meta name="twitter:player:height" content="720">
<meta name="robots" content="${clip.privacy === 'public' ? 'index' : 'noindex'}">
<style>
body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;font-family:system-ui,sans-serif;color:#fff;
background:radial-gradient(circle at 20% 20%,#5b2bd6 0,transparent 45%),radial-gradient(circle at 80% 80%,#c21fe0 0,transparent 40%),#0b0614}
main{width:min(1100px,94vw);padding:18px;border-radius:18px;background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.18);
backdrop-filter:blur(20px);box-shadow:0 30px 80px rgba(0,0,0,.45)}
video{width:100%;border-radius:12px;background:#000;display:block}
h1{font-size:20px;margin:14px 4px 2px}p{margin:0 4px;color:rgba(255,255,255,.65);font-size:14px}
footer{display:flex;justify-content:space-between;align-items:center;margin-top:10px}
button{background:none;border:0;color:rgba(255,255,255,.5);cursor:pointer;font-size:12px}
</style></head><body><main>
<video src="${video}" controls autoplay playsinline></video>
<h1>${title}</h1>
<footer><p>Clipped by ${owner} with ${app} · ${clip.views + 1} views</p><button id="r">Report</button></footer>
</main><script>
document.getElementById('r').onclick=async()=>{const reason=prompt('What is wrong with this clip?');if(!reason)return;
await fetch('/c/${id}/report',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({reason})});alert('Thanks, we will take a look.')};
</script></body></html>`);
}

async function streamClip(request, env, id) {
  const clip = await viewableClip(env, id);
  if (!clip) return new Response('Not found', { status: 404 });
  const key = `clips/${id}.mp4`;
  const range = request.headers.get('range');
  let obj;
  let status = 200;
  const headers = new Headers({ 'content-type': 'video/mp4', 'accept-ranges': 'bytes', 'cache-control': 'public, max-age=3600' });
  const m = range && /^bytes=(\d*)-(\d*)$/.exec(range);
  if (m) {
    const size = clip.size_bytes;
    let start = m[1] === '' ? Math.max(0, size - parseInt(m[2], 10)) : parseInt(m[1], 10);
    let end = m[1] !== '' && m[2] !== '' ? Math.min(parseInt(m[2], 10), size - 1) : size - 1;
    if (start >= size || start > end) return new Response('Bad range', { status: 416, headers: { 'content-range': `bytes */${size}` } });
    obj = await env.CLIPS.get(key, { range: { offset: start, length: end - start + 1 } });
    status = 206;
    headers.set('content-range', `bytes ${start}-${end}/${size}`);
    headers.set('content-length', String(end - start + 1));
  } else {
    obj = await env.CLIPS.get(key);
    if (obj) headers.set('content-length', String(obj.size));
  }
  if (!obj) return new Response('Not found', { status: 404 });
  return new Response(obj.body, { status, headers });
}

async function report(request, env, id) {
  if (limited(request, 'report', 10, 3600)) return fail(429, 'Too many reports.');
  const b = await request.json().catch(() => ({}));
  const reason = String(b.reason || '').slice(0, 500);
  if (!reason) return fail(400, 'Say what is wrong.');
  const ipHash = await sha256(request.headers.get('cf-connecting-ip') || '');
  await env.DB.prepare('INSERT INTO reports (clip_id, reason, created_at, ip_hash) VALUES (?, ?, ?, ?)').bind(id, reason, now(), ipHash).run();
  return json({ ok: true });
}

function page(title, text) {
  return `<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${escapeHtml(title)}</title><style>body{margin:0;height:100vh;display:flex;align-items:center;justify-content:center;font-family:system-ui,sans-serif;
color:#fff;background:radial-gradient(circle at 25% 25%,#5b2bd6 0,transparent 45%),#0b0614}div{padding:32px 40px;border-radius:18px;
background:rgba(255,255,255,.08);border:1px solid rgba(255,255,255,.18);text-align:center}p{color:rgba(255,255,255,.65)}</style></head>
<body><div><h1>${escapeHtml(title)}</h1><p>${escapeHtml(text)}</p></div></body></html>`;
}

// ---------------------------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------------------------

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const path = url.pathname;
    const method = request.method;
    try {
      let m;
      if (method === 'POST' && path === '/auth/register') return await register(request, env);
      if (method === 'POST' && path === '/auth/login') return await login(request, env);
      if (method === 'GET' && path === '/admin') return html(adminPage());
      if (path.startsWith('/admin/')) return await adminApi(request, env, path, method);
      if (method === 'POST' && path === '/auth/refresh') return await refreshTokens(request, env);
      if (method === 'POST' && path === '/auth/logout') return await logout(request, env);

      if ((m = /^\/c\/([A-Za-z0-9]{10})\.mp4$/.exec(path)) && (method === 'GET' || method === 'HEAD')) return await streamClip(request, env, m[1]);
      if (method === 'GET' && (m = /^\/c\/([A-Za-z0-9]{10})$/.exec(path))) return await sharePage(env, m[1], url.origin, ctx);
      if (method === 'POST' && (m = /^\/c\/([A-Za-z0-9]{10})\/report$/.exec(path))) return await report(request, env, m[1]);

      if (!env.CLIPS && (path.startsWith('/c/') || (path.startsWith('/clips') && method !== 'GET' && method !== 'DELETE')))
        return fail(503, 'Cloud clips are turned off on this server.');
      if (path === '/me' || path.startsWith('/me/') || path.startsWith('/clips')) {
        const user = await authUser(request, env);
        if (!user) return fail(401, 'Signed out. Sign in again.');
        if (method === 'GET' && path === '/me') return json(publicUser(user, env));
        if (method === 'POST' && path === '/me/password') return await changePassword(request, env, user);
        if (method === 'GET' && path === '/clips') return await myClips(env, user, url.origin);
        if (method === 'POST' && path === '/clips') return await createClip(request, env, user);
        if (method === 'PUT' && (m = /^\/clips\/([A-Za-z0-9]{10})\/upload$/.exec(path))) return await uploadClip(request, env, user, m[1], url.origin);
        if (method === 'PATCH' && (m = /^\/clips\/([A-Za-z0-9]{10})$/.exec(path))) return await updateClip(request, env, user, m[1], url.origin);
        if (method === 'DELETE' && (m = /^\/clips\/([A-Za-z0-9]{10})$/.exec(path))) return await deleteClip(env, user, m[1]);
      }
      if (path === '/') return html(page(env.APP_NAME || 'Replayr', 'This is the Replayr server. Open the app to sign in.'));
      return fail(404, 'Not found.');
    } catch (e) {
      // Never log tokens or request bodies: just what failed.
      console.error('request failed', path, e && e.message);
      return fail(500, 'Something went wrong on the server. Try again.');
    }
  },
};
