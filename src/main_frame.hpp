#pragma once
#include <wx/wx.h>
#include <wx/fswatcher.h>
#include <wx/listctrl.h>
#include <wx/simplebook.h>
#include <wx/taskbar.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "api.hpp"
#include "client_update.hpp"
#include "desktop.hpp"
#include "store.hpp"
#include "ui.hpp"

namespace loki {

/**
 * The single window: connecting to a server (pairing or password), then the list
 * of server folders and where each lives on this computer. Closing it keeps LOKI
 * Sync running in the notification area.
 *
 * Syncs start as soon as something changes: a long-poll on the server's watch
 * endpoint reports other devices' changes, and a file watcher on the mapped
 * folders reports local ones once writing has settled. A slow timer is the
 * fallback for anything either misses.
 */
class MainFrame : public wxFrame {
public:
    explicit MainFrame(Store& store);
    ~MainFrame() override;

    void SyncNow();
    void ShowWindow();
    void BrowseFiles();
    void Quit();

private:
    // Runs `job` on a worker thread, then `done` on the UI thread with its error, if any.
    void Background(std::function<void()> job, std::function<void(std::optional<std::string>)> done);

#if defined(_WIN32) && defined(LOKI_HAS_SKALD)
    void CheckClientUpdates(bool manual);
    void OfferClientUpdate(bool manual);
    void TryInstallClientUpdate();
    wxTimer updates_{this};
    std::optional<ClientUpdate> available_update_;
    std::shared_ptr<PreparedClientUpdate> prepared_update_;
    std::string offered_version_;
    int update_menu_id_ = wxID_NONE;
    bool checking_updates_ = false;
#endif

    wxPanel* BuildConnect(wxWindow* parent);
    wxPanel* BuildFolders(wxWindow* parent);
    wxPanel* BuildWorkspace(wxWindow* parent);
    void BuildMenus();
    void OpenPage(int index);
    void ShowConnectStep();
    void ShowAccountMenu(wxPoint where);
    void ShowAbout();

    void OnContinue();
    void OnPair();
    void PollPairing(SignIn pairing);
    void OnLogin();
    void ProfileChanged();
    void ShowSession();

    /** (Re)starts the server watch for the signed-in account. */
    void StartWatching();
    void StopWatching();
    /** Points the file watcher at the currently mapped folders. */
    void WatchFolders();

    void Refresh();
    void Populate();
    std::optional<std::string> SelectedFolder() const;
    void UpdateFolderButtons();
    void OnChooseLocal();
    void OnStop();
    /** Applies a choice ("restore" or "delete") for files a sync found missing. */
    void OnMissing(const char* decision);
    void OnNewFolder();
    void OnSignOut();

    Store& store_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> syncing_{false};
    // A change arrived while syncing; sync again when this pass ends.
    bool rerun_ = false;
    bool quitting_ = false;
    std::shared_ptr<std::atomic<bool>> watching_;

    wxSimplebook* book_ = nullptr;
    wxSimplebook* pages_ = nullptr;
    ui::Sidebar* sidebar_ = nullptr;
    wxTimer timer_;
    wxTimer settle_;
    std::string profile_token_;
    std::map<std::string, Mapping> shown_mappings_;
    std::unique_ptr<wxFileSystemWatcher> files_;
    std::unique_ptr<wxTaskBarIcon> tray_;

    // Connect page.
    wxTextCtrl* server_ = nullptr;
    wxButton* continue_ = nullptr;
    wxButton* back_ = nullptr;
    wxPanel* signin_ = nullptr;
    wxStaticText* server_name_ = nullptr;
    wxTextCtrl* username_ = nullptr;
    wxTextCtrl* password_ = nullptr;
    wxTextCtrl* code_ = nullptr;
    wxPanel* pairing_panel_ = nullptr;
    wxStaticText* user_code_ = nullptr;
    wxString approval_url_;
    ui::Status* connect_status_ = nullptr;
    wxStaticText* http_warning_ = nullptr;
    std::optional<std::string> discovered_;
    std::shared_ptr<std::atomic<bool>> pairing_alive_;

    // Folders page.
    wxStaticText* account_ = nullptr;
    wxGauge* quota_ = nullptr;
    wxStaticText* quota_text_ = nullptr;
    wxListCtrl* list_ = nullptr;
    wxButton* choose_local_ = nullptr;
    wxButton* stop_ = nullptr;
    wxButton* restore_missing_ = nullptr;
    wxButton* delete_missing_ = nullptr;
    ui::Status* status_ = nullptr;
    std::vector<Folder> folders_;
};

}  // namespace loki
