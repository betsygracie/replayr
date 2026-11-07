# Replayr account server

Invite-only accounts with a username and password (no emails). Runs on Cloudflare Workers + D1,
which are free and don't need a card.

- **Making an account** needs an invite code. Only you can make codes, at `https://<server>/admin`,
  using your admin key.
- **Passwords** are stored as PBKDF2-SHA256 hashes with a per-user salt. The app keeps only a
  session token (in Windows Credential Manager), never the password.
- **Cloud clips** (upload + share links) need R2, which is free up to 10 GB but asks for a card.
  It's off by default; see `wrangler.toml` to turn it on.

## Deploying

The `Deploy account server` GitHub Action does everything (database, schema, deploy, secrets) when
`server/` changes. It needs three repository secrets:

| Secret | What |
| --- | --- |
| `CLOUDFLARE_API_TOKEN` | Cloudflare API token from the "Edit Cloudflare Workers" template, plus D1 Edit |
| `CLOUDFLARE_ACCOUNT_ID` | Your Cloudflare account id |
| `ADMIN_KEY` | A long password of your choice (16+ characters) for the invite-code page |

The server address is written to `server/URL.txt` after each deploy.
