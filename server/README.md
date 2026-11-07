# Replayr account server

Sign in with Discord or Steam, upload clips, and share links that play in the browser and embed in
Discord. Everything runs on Cloudflare's free plan:

- **Workers** runs `src/worker.js`
- **D1** is the database (`schema.sql`)
- **R2** stores the clips; downloads (people watching shared clips) are free

No passwords or email addresses are stored. Refresh tokens are only stored as hashes.

## One-time setup (about 15 minutes)

You need a free Cloudflare account, Node.js 18 or newer, and a Discord account.

1. In this folder: `npm install -g wrangler`, then `wrangler login`.
2. Create the database: `wrangler d1 create replayr`. Copy the `database_id` it prints into
   `wrangler.toml`. Then create the tables: `wrangler d1 execute replayr --remote --file=schema.sql`
3. Create the clip bucket: `wrangler r2 bucket create replayr-clips`
   (R2 asks you to add a payment method once, even on the free tier.)
4. Deploy once to get your address: `wrangler deploy`. It prints something like
   `https://replayr.<your-name>.workers.dev`. That's your **server URL**.
5. **Discord app:** https://discord.com/developers/applications > New Application > OAuth2.
   - Add the redirect `https://replayr.<your-name>.workers.dev/auth/discord/callback`
   - Copy the Client ID and Client Secret.
6. **Steam key:** sign in at https://steamcommunity.com/dev/apikey and register your workers.dev
   domain. This is only used to show Steam names and avatars; sign-in works without it.
7. Set the secrets (each command asks for the value):
   ```
   wrangler secret put DISCORD_CLIENT_ID
   wrangler secret put DISCORD_CLIENT_SECRET
   wrangler secret put STEAM_API_KEY
   wrangler secret put TOKEN_SECRET        # paste any long random string
   ```
8. `wrangler deploy` again.
9. In the app, set `CS_ACCOUNT_SERVER` in `src/AppInfo.h` to your server URL and rebuild.

## Free-plan limits worth knowing

- **Upload size:** each upload is at most about 95 MB (Workers accept 100 MB request bodies on
  the free plan). The app suggests the "Fit under a size" export for bigger clips.
- **Storage:** each user gets 500 MB of cloud storage (the `quota_bytes` column), and R2's free
  tier is 10 GB in total. If you go past it, R2 charges per GB stored. Raise or lower the per-user
  quota to match.
- **Requests:** Workers allow 100,000 requests a day for free. Every clip view is one or a few requests.

## Moderation

Reports from the "Report" link on share pages land in the `reports` table:

```
wrangler d1 execute replayr --remote --command "SELECT * FROM reports ORDER BY created_at DESC LIMIT 50"
```

To remove a clip, set it to private: `UPDATE clips SET privacy='private' WHERE id='...'`.
To ban a user, set `UPDATE users SET banned=1 WHERE id='...'`. Banned users' clips stop loading
and they can't sign in.

## Before going public

Have someone review this code and your Terms of Service / Privacy Policy. Public sharing makes
this a user-to-user service under the UK Online Safety Act, so do Ofcom's illegal content risk
assessment and keep an eye on the reports.
