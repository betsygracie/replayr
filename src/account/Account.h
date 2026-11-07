#pragma once
// Sign in with Discord or Steam (through the Replayr server), keep the session in Windows
// Credential Manager, and upload clips in the background.
//
// Sign-in uses the system browser with PKCE and a one-shot listener on 127.0.0.1:<random port>,
// so the app never sees a Discord or Steam password. All network work runs on one worker thread;
// the UI reads snapshots and polls events.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cs {

enum class AccountState { Off, SignedOut, SigningIn, SignedIn };

struct AccountUser {
    std::string id, name, provider, avatarUrl;
    uint64_t usedBytes = 0, quotaBytes = 0;
};

struct CloudClip {
    std::string id, title, privacy, url;   // privacy: public / unlisted / private
    uint64_t sizeBytes = 0;
    double duration = 0.0;
    int64_t views = 0, createdAt = 0;
};

struct UploadItem {
    std::wstring path;
    std::string title;
    float progress = 0.0f;   // 0..1
    bool active = false;
};

enum class AccountEventType { SignedIn, SignedOut, SignInFailed, Uploaded, UploadFailed, Error };
struct AccountEvent {
    AccountEventType type;
    std::string message;     // error text, or the share link for Uploaded
    std::string title;
};

class Account {
public:
    Account();
    ~Account();
    static bool Enabled();                  // a server address is set
    // Server address: CS_ACCOUNT_SERVER, unless settings.json has "accountServer". Call before Start.
    static void SetServer(const std::string& url);

    void Start(const std::wstring& cacheDir);   // restores a saved session
    AccountState State() const;
    AccountUser User() const;
    std::wstring AvatarPath() const;        // downloaded avatar image ("" until it's there)

    void SignIn(const std::string& provider);   // "discord" / "steam": opens the browser
    void CancelSignIn();
    void SignOut();

    void RefreshClips();
    std::vector<CloudClip> Clips() const;
    bool ClipsLoading() const;
    void SetPrivacy(const std::string& id, const std::string& privacy);
    void DeleteClip(const std::string& id);

    void Upload(const std::wstring& path, const std::string& title, double duration, const std::string& privacy);
    std::vector<UploadItem> Uploads() const;

    bool PollEvent(AccountEvent& out);

    struct Impl;

private:
    std::unique_ptr<Impl> d_;
};

} // namespace cs
