// Replayr account server: sign in with Discord or Steam, upload clips, share links.
// Runs on Cloudflare Workers (free plan) with D1 (database) and R2 (clip files).
//
// Sign-in (no passwords or emails are ever handled here):
//   1. The app opens the browser at /auth/<discord|steam>/start?port=P&state=S&challenge=C, where C is
//      base64url(SHA-256(verifier)) and the app keeps the verifier (PKCE).
//   2. The provider signs the user in and sends them back to /auth/<provider>/callback.
//   3. We create a one-time code bound to C and send the browser to http://127.0.0.1:P/callback.
//   4. The app posts the code + verifier to /auth/token and gets an access token (1 hour) and a
//      refresh token (60 days, rotated on every use, stored only as a hash).

const ACCESS_TTL = 3600;
const REFRESH_TTL = 60 * 86400;
const CODE_TTL = 120;
const PENDING_TTL = 600;

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

function publicUser(u) {
  return { id: u.id, name: u.display_name, avatar: u.avatar_url, provider: u.provider, usedBytes: u.used_bytes, quotaBytes: u.quota_bytes };
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

async function startLogin(request, env, url, provider) {
  if (limited(request, 'start', 30, 600)) return fail(429, 'Too many sign-in attempts. Try again in a few minutes.');
  const port = parseInt(url.searchParams.get('port') || '', 10);
  const appState = url.searchParams.get('state') || '';
  const challenge = url.searchParams.get('challenge') || '';
  if (!(port >= 1024 && port <= 65535) || !/^[A-Za-z0-9_-]{16,128}$/.test(appState) || !/^[A-Za-z0-9_-]{43}$/.test(challenge))
    return fail(400, 'Bad sign-in request.');
  const state = randomToken(24);
  await env.DB.prepare('DELETE FROM pending WHERE expires_at < ?').bind(now()).run();
  await env.DB.prepare('INSERT INTO pending (state, port, app_state, challenge, expires_at) VALUES (?, ?, ?, ?, ?)')
    .bind(state, port, appState, challenge, now() + PENDING_TTL).run();
  const origin = url.origin;
  if (provider === 'discord') {
    const q = new URLSearchParams({
      response_type: 'code', client_id: env.DISCORD_CLIENT_ID, scope: 'identify', state,
      redirect_uri: `${origin}/auth/discord/callback`, prompt: 'none',
    });
    return Response.redirect(`https://discord.com/oauth2/authorize?${q}`, 302);
  }
  const q = new URLSearchParams({
    'openid.ns': 'http://specs.openid.net/auth/2.0',
    'openid.mode': 'checkid_setup',
    'openid.return_to': `${origin}/auth/steam/callback?state=${state}`,
    'openid.realm': origin,
    'openid.identity': 'http://specs.openid.net/auth/2.0/identifier_select',
    'openid.claimed_id': 'http://specs.openid.net/auth/2.0/identifier_select',
  });
  return Response.redirect(`https://steamcommunity.com/openid/login?${q}`, 302);
}

async function takePending(env, state) {
  if (!state) return null;
  const p = await env.DB.prepare('SELECT * FROM pending WHERE state = ?').bind(state).first();
  if (!p) return null;
  await env.DB.prepare('DELETE FROM pending WHERE state = ?').bind(state).run();
  return p.expires_at >= now() ? p : null;
}

async function upsertUser(env, provider, providerId, name, avatar) {
  const existing = await env.DB.prepare('SELECT * FROM users WHERE provider = ? AND provider_id = ?').bind(provider, providerId).first();
  if (existing) {
    await env.DB.prepare('UPDATE users SET display_name = ?, avatar_url = ? WHERE id = ?').bind(name, avatar, existing.id).run();
    return { ...existing, display_name: name, avatar_url: avatar };
  }
  const id = randomToken(12);
  await env.DB.prepare('INSERT INTO users (id, provider, provider_id, display_name, avatar_url, created_at) VALUES (?, ?, ?, ?, ?, ?)')
    .bind(id, provider, providerId, name, avatar, now()).run();
  return await env.DB.prepare('SELECT * FROM users WHERE id = ?').bind(id).first();
}

async function finishLogin(env, user, pending) {
  if (user.banned) return html(page('Account suspended', 'This account can no longer sign in.'), 403);
  const code = randomToken(32);
  await env.DB.prepare('DELETE FROM login_codes WHERE expires_at < ?').bind(now()).run();
  await env.DB.prepare('INSERT INTO login_codes (code_hash, user_id, challenge, expires_at) VALUES (?, ?, ?, ?)')
    .bind(await sha256(code), user.id, pending.challenge, now() + CODE_TTL).run();
  const q = new URLSearchParams({ code, state: pending.app_state });
  return Response.redirect(`http://127.0.0.1:${pending.port}/callback?${q}`, 302);
}

async function discordCallback(env, url) {
  const pending = await takePending(env, url.searchParams.get('state'));
  if (!pending) return html(page('Sign-in expired', 'Go back to Replayr and try again.'), 400);
  const code = url.searchParams.get('code');
  if (!code) return html(page('Sign-in cancelled', 'You can close this tab.'), 400);
  const tokenRes = await fetch('https://discord.com/api/oauth2/token', {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      client_id: env.DISCORD_CLIENT_ID, client_secret: env.DISCORD_CLIENT_SECRET, grant_type: 'authorization_code',
      code, redirect_uri: `${url.origin}/auth/discord/callback`,
    }),
  });
  if (!tokenRes.ok) return html(page("Couldn't sign in", 'Discord said no. Go back to Replayr and try again.'), 502);
  const { access_token } = await tokenRes.json();
  const meRes = await fetch('https://discord.com/api/users/@me', { headers: { authorization: `Bearer ${access_token}` } });
  if (!meRes.ok) return html(page("Couldn't sign in", 'Discord did not share your profile.'), 502);
  const me = await meRes.json();
  const avatar = me.avatar ? `https://cdn.discordapp.com/avatars/${me.id}/${me.avatar}.png?size=128` : null;
  const user = await upsertUser(env, 'discord', me.id, (me.global_name || me.username || 'Discord user').slice(0, 64), avatar);
  return finishLogin(env, user, pending);
}

async function steamCallback(env, url) {
  const pending = await takePending(env, url.searchParams.get('state'));
  if (!pending) return html(page('Sign-in expired', 'Go back to Replayr and try again.'), 400);
  const p = url.searchParams;
  if (p.get('openid.mode') !== 'id_res') return html(page('Sign-in cancelled', 'You can close this tab.'), 400);
  // Only Steam's endpoint, and only for our own return URL.
  if (p.get('openid.op_endpoint') !== 'https://steamcommunity.com/openid/login' || !(p.get('openid.return_to') || '').startsWith(`${url.origin}/auth/steam/callback`))
    return html(page("Couldn't sign in", 'That sign-in response was not from Steam.'), 400);
  const m = /^https:\/\/steamcommunity\.com\/openid\/id\/(\d{17})$/.exec(p.get('openid.claimed_id') || '');
  if (!m) return html(page("Couldn't sign in", 'Steam did not send an account id.'), 400);
  // Ask Steam to confirm the signature.
  const check = new URLSearchParams();
  for (const [k, v] of p) if (k.startsWith('openid.')) check.set(k, v);
  check.set('openid.mode', 'check_authentication');
  const res = await fetch('https://steamcommunity.com/openid/login', {
    method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: check,
  });
  if (!res.ok || !/is_valid\s*:\s*true/.test(await res.text())) return html(page("Couldn't sign in", 'Steam could not confirm the sign-in.'), 400);
  const steamId = m[1];
  let name = 'Steam user', avatar = null;
  if (env.STEAM_API_KEY) {
    const s = await fetch(`https://api.steampowered.com/ISteamUser/GetPlayerSummaries/v2/?key=${env.STEAM_API_KEY}&steamids=${steamId}`);
    if (s.ok) {
      const player = (await s.json())?.response?.players?.[0];
      if (player) {
        name = (player.personaname || name).slice(0, 64);
        avatar = player.avatarfull || player.avatarmedium || null;
      }
    }
  }
  const user = await upsertUser(env, 'steam', steamId, name, avatar);
  return finishLogin(env, user, pending);
}

async function issueTokens(env, user, device) {
  const refresh = randomToken(32);
  const t = now();
  await env.DB.prepare('INSERT INTO sessions (refresh_hash, user_id, device, created_at, last_used_at, expires_at) VALUES (?, ?, ?, ?, ?, ?)')
    .bind(await sha256(refresh), user.id, (device || 'PC').slice(0, 64), t, t, t + REFRESH_TTL).run();
  const access = await signJwt({ sub: user.id, exp: t + ACCESS_TTL }, env.TOKEN_SECRET);
  return { accessToken: access, refreshToken: refresh, expiresIn: ACCESS_TTL, user: publicUser(user) };
}

async function exchangeCode(request, env) {
  if (limited(request, 'token', 30, 600)) return fail(429, 'Too many attempts.');
  const body = await request.json().catch(() => ({}));
  if (typeof body.code !== 'string' || typeof body.verifier !== 'string') return fail(400, 'Bad request.');
  const hash = await sha256(body.code);
  const row = await env.DB.prepare('SELECT * FROM login_codes WHERE code_hash = ?').bind(hash).first();
  if (row) await env.DB.prepare('DELETE FROM login_codes WHERE code_hash = ?').bind(hash).run();
  if (!row || row.expires_at < now() || (await sha256(body.verifier)) !== row.challenge) return fail(400, 'That sign-in has expired. Try again.');
  const user = await env.DB.prepare('SELECT * FROM users WHERE id = ?').bind(row.user_id).first();
  if (!user || user.banned) return fail(403, 'This account can no longer sign in.');
  return json(await issueTokens(env, user, body.device));
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
  return json({ clips: rows.results.map((c) => publicClip(c, origin)), user: publicUser(user) });
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
  await env.CLIPS.delete(`clips/${id}.mp4`);
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
      if (method === 'GET' && (m = /^\/auth\/(discord|steam)\/start$/.exec(path))) return await startLogin(request, env, url, m[1]);
      if (method === 'GET' && path === '/auth/discord/callback') return await discordCallback(env, url);
      if (method === 'GET' && path === '/auth/steam/callback') return await steamCallback(env, url);
      if (method === 'POST' && path === '/auth/token') return await exchangeCode(request, env);
      if (method === 'POST' && path === '/auth/refresh') return await refreshTokens(request, env);
      if (method === 'POST' && path === '/auth/logout') return await logout(request, env);

      if ((m = /^\/c\/([A-Za-z0-9]{10})\.mp4$/.exec(path)) && (method === 'GET' || method === 'HEAD')) return await streamClip(request, env, m[1]);
      if (method === 'GET' && (m = /^\/c\/([A-Za-z0-9]{10})$/.exec(path))) return await sharePage(env, m[1], url.origin, ctx);
      if (method === 'POST' && (m = /^\/c\/([A-Za-z0-9]{10})\/report$/.exec(path))) return await report(request, env, m[1]);

      if (path === '/me' || path.startsWith('/clips')) {
        const user = await authUser(request, env);
        if (!user) return fail(401, 'Signed out. Sign in again.');
        if (method === 'GET' && path === '/me') return json(publicUser(user));
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
