#pragma once

// Change the app's name here: window title, tray, folders and registry entries all use it.
#define CS_APP_NAME     "Replayr"
#define CS_APP_NAME_W   L"Replayr"
#define CS_APP_VERSION  "0.2.0"

#define CS_WINDOW_CLASS L"ReplayrMainWindow"
#define CS_MUTEX_NAME   L"Local\\Replayr.SingleInstance"
#define CS_SHOW_MESSAGE L"Replayr.ShowWindow"

// Updates come from the latest GitHub release of this repository ("owner/name"). The release needs
// a tag like "v0.2.0" and the new Replayr.exe attached as an asset. Leave empty to turn the
// update checker off.
#define CS_UPDATE_REPO  "betsygracie/replayr"

// The account server (server/ in the repo, deployed to Cloudflare), e.g.
// "https://replayr.yourname.workers.dev". Leave empty to hide accounts and keep the storage card.
#define CS_ACCOUNT_SERVER ""
