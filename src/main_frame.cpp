#include "main_frame.hpp"

#include <wx/aboutdlg.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/iconbndl.h>
#include <wx/numdlg.h>
#include <wx/platinfo.h>
#include <wx/statline.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#ifdef __WXMSW__
#include <wx/msw/registry.h>
#endif

#include <chrono>
#include <thread>

#include "engine.hpp"
#include "commands.hpp"
#include "workspace_pages.hpp"
#include "files_dialog.hpp"
#include "files_api.hpp"

namespace loki {
namespace {

wxString W(const std::string& utf8) { return wxString::FromUTF8(utf8); }
std::string U(const wxString& text) { return text.utf8_string(); }

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

const std::pair<const char*, const char*> kinds[] = {
    {"retroarch-saves", "RetroArch saves"},   {"retroarch-states", "RetroArch save states"},
    {"emulator-saves", "Other emulator saves"}, {"pc-game-saves", "PC game saves"},
    {"documents", "Documents"},                {"generic", "General files"},
};
wxString kind_label(const std::string& kind) {
    for (const auto& [value, label] : kinds) {
        if (kind == value) return label;
    }
    return W(kind);
}

const wxString kPlainHttpWarning =
    "This address uses plain HTTP, so your sign-in and files travel unencrypted. Use HTTPS unless the "
    "server is on your own network.";
bool plain_http(const wxString& address) { return address.Strip(wxString::both).Lower().StartsWith("http://"); }

wxString size_text(std::int64_t bytes) { return wxFileName::GetHumanReadableSize(wxULongLong(bytes), "0 B", 1); }

wxString when(std::int64_t ms) {
    if (ms <= 0) return "Never";
    const auto minutes = (now_ms() - ms) / 60000;
    if (minutes < 1) return "Just now";
    if (minutes < 60) return wxString::Format("%lld min ago", static_cast<long long>(minutes));
    return wxDateTime(wxLongLong(ms)).Format("%Y-%m-%d %H:%M");
}

using ui::tree_icon;

// Menu commands shared by the menu bar, the account menu, and the tray.
enum Command {
    kBrowseFiles = wxID_HIGHEST + 1,
    kNewFolder,
    kOpenDashboard,
    kReconnect,
    kSignOut,
    kAutostart,
    kCheckUpdates,
    kFirstPage,  // One command per page follows.
};
const wxString kPages[] = {"Save sync", "Browse files", "Updates", "Repositories"};

class Tray : public wxTaskBarIcon {
public:
    explicit Tray(MainFrame* frame) : frame_(frame) {
        Bind(wxEVT_TASKBAR_LEFT_DCLICK, [this](wxTaskBarIconEvent&) { frame_->ShowWindow(); });
    }

protected:
    wxMenu* CreatePopupMenu() override {
        auto* menu = new wxMenu;
        menu->Append(wxID_OPEN, "Open LOKI Client");
        menu->Append(wxID_REFRESH, "Sync now");
        menu->Append(kBrowseFiles, wxS("Browse files…"));
        menu->AppendSeparator();
        menu->Append(wxID_EXIT, "Quit");
        menu->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
            if (e.GetId() == wxID_OPEN) frame_->ShowWindow();
            if (e.GetId() == wxID_REFRESH) frame_->SyncNow();
            if (e.GetId() == kBrowseFiles) frame_->BrowseFiles();
            if (e.GetId() == wxID_EXIT) frame_->Quit();
        });
        return menu;
    }

private:
    MainFrame* frame_;
};

}  // namespace

MainFrame::MainFrame(Store& store)
    : wxFrame(nullptr, wxID_ANY, "LOKI Client"), store_(store), timer_(this), settle_(this) {
    SetSize(FromDIP(wxSize(1090, 680)));
    wxIconBundle icons;
    for (const int size : {16, 24, 32, 48, 64}) icons.AddIcon(tree_icon(size));
    SetIcons(icons);
    SetMinSize(FromDIP(wxSize(850, 520)));
    SetBackgroundColour(ui::page_background());
    BuildMenus();
    book_ = new wxSimplebook(this);
    book_->AddPage(BuildConnect(book_), "Connect");
    book_->AddPage(BuildWorkspace(book_), "Workspace");
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(book_, 1, wxEXPAND);
    SetSizer(sizer);

    if (wxTaskBarIcon::IsAvailable()) {
        tray_ = std::make_unique<Tray>(this);
        tray_->SetIcon(tree_icon(32), "LOKI Client");
    }
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        // Closing hides to the notification area; syncing carries on.
        if (tray_ && !quitting_ && e.CanVeto()) {
            e.Veto();
            Hide();
            return;
        }
        Destroy();
    });
    // Both timers sync: the slow fallback, and the one that waits for local writes to settle.
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { SyncNow(); }, timer_.GetId());
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { SyncNow(); }, settle_.GetId());
    timer_.Start(5 * 60 * 1000);
    Bind(wxEVT_FSWATCHER, [this](wxFileSystemWatcherEvent& e) {
        if (e.GetChangeType() & (wxFSW_EVENT_ACCESS | wxFSW_EVENT_WARNING | wxFSW_EVENT_ERROR)) return;
        if (e.GetPath().GetFullName().StartsWith(".loki")) return;
        settle_.StartOnce(int(Engine::settle_ms) + 500);
    });

    profile_token_ = store_.token();
    shown_mappings_ = store_.mappings();
    if (store_.signed_in()) {
        book_->SetSelection(1);
        Refresh();
        SyncNow();
        StartWatching();
        // The file watcher needs a running event loop.
        CallAfter([this] { WatchFolders(); OpenPage(store_.can_sync() ? 0 : 1); });
    } else {
        book_->SetSelection(0);
        server_->SetValue(W(store_.server()));
    }
    Centre();
#if defined(_WIN32) && defined(LOKI_HAS_SKALD)
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (prepared_update_) TryInstallClientUpdate();
        else CheckClientUpdates(false);
    }, updates_.GetId());
    updates_.StartOnce(3000);
#endif
}

void MainFrame::BuildMenus() {
    auto* file = new wxMenu;
    file->Append(wxID_REFRESH, "&Sync now\tCtrl+R");
    file->Append(kNewFolder, wxS("&New sync folder…"));
#ifdef __WXMSW__
    file->AppendSeparator();
    file->AppendCheckItem(kAutostart, "Start at sign-&in");
    file->Check(kAutostart, starts_at_sign_in());
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
        try { set_start_at_sign_in(e.IsChecked()); }
        catch (const std::exception& error) { wxMessageBox(W(error.what()), "Start at sign-in", wxICON_ERROR, this); }
        GetMenuBar()->Check(kAutostart, starts_at_sign_in());
    }, kAutostart);
#endif
    file->AppendSeparator();
    file->Append(wxID_CLOSE, "&Close window\tCtrl+W");
    file->Append(wxID_EXIT, "&Quit\tCtrl+Q");

    auto* view = new wxMenu;
    for (int i = 0; i < 4; ++i)
        view->Append(kFirstPage + i, wxString::Format("%s\tCtrl+%d", kPages[i], i + 1));

    auto* account = new wxMenu;
    account->Append(kOpenDashboard, "Open LOKI in your &browser");
    account->Append(kReconnect, wxS("&Reconnect to LOKI…"));
    account->AppendSeparator();
    account->Append(kSignOut, wxS("&Sign out…"));

    auto* help = new wxMenu;
#if defined(__WXMSW__) && defined(LOKI_HAS_SKALD)
    update_menu_id_ = kCheckUpdates;
    help->Append(kCheckUpdates, wxS("Check for client &updates…"));
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { CheckClientUpdates(true); }, kCheckUpdates);
    help->AppendSeparator();
#endif
    help->Append(wxID_ABOUT, "&About LOKI Client");

    auto* bar = new wxMenuBar;
    bar->Append(file, "&File");
    bar->Append(view, "&View");
    bar->Append(account, "&Account");
    bar->Append(help, "&Help");
    SetMenuBar(bar);

    // Account commands only make sense once signed in.
    Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent& e) {
        e.Skip();
        const bool signed_in = store_.signed_in();
        auto* menus = GetMenuBar();
        for (const int id : {int(wxID_REFRESH), int(kNewFolder), int(kOpenDashboard), int(kReconnect), int(kSignOut)})
            menus->Enable(id, signed_in);
        for (int i = 0; i < 4; ++i) menus->Enable(kFirstPage + i, signed_in);
    });
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Refresh(); SyncNow(); }, wxID_REFRESH);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (store_.signed_in()) OnNewFolder(); }, kNewFolder);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { BrowseFiles(); }, kBrowseFiles);
    Bind(wxEVT_MENU, [this](wxCommandEvent& e) { OpenPage(e.GetId() - kFirstPage); }, kFirstPage, kFirstPage + 3);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (store_.signed_in()) wxLaunchDefaultBrowser(W(store_.server()));
    }, kOpenDashboard);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (!store_.signed_in()) return;
        if (pairing_alive_) *pairing_alive_ = false;
        server_->SetValue(W(store_.server()));
        discovered_.reset();
        connect_status_->Clear();
        ShowConnectStep();
        book_->SetSelection(0);
        OnContinue();
    }, kReconnect);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (store_.signed_in()) OnSignOut(); }, kSignOut);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_CLOSE);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Quit(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ShowAbout(); }, wxID_ABOUT);
}

void MainFrame::ShowAccountMenu(wxPoint where) {
    wxMenu menu;
    menu.Append(kOpenDashboard, "Open LOKI in your browser");
    menu.Append(kReconnect, wxS("Reconnect to LOKI…"));
    menu.AppendSeparator();
    menu.Append(kSignOut, wxS("Sign out…"));
    sidebar_->PopupMenu(&menu, where);
}

void MainFrame::ShowAbout() {
    wxAboutDialogInfo about;
    about.SetName("LOKI Client");
    about.SetVersion(LOKI_CLIENT_VERSION);
    about.SetDescription("Save sync, shared files, apps, and Skald repositories from your LOKI server.");
    about.SetWebSite("https://github.com/LOKI-Server/loki-desktop");
    about.SetIcon(tree_icon(64));
    wxAboutBox(about, this);
}

MainFrame::~MainFrame() {
    *alive_ = false;
    if (pairing_alive_) *pairing_alive_ = false;
    StopWatching();
    timer_.Stop();
    settle_.Stop();
#if defined(_WIN32) && defined(LOKI_HAS_SKALD)
    updates_.Stop();
#endif
    if (tray_) tray_->RemoveIcon();
}

void MainFrame::Background(std::function<void()> job, std::function<void(std::optional<std::string>)> done) {
    std::thread([alive = alive_, job = std::move(job), done = std::move(done), this] {
        std::optional<std::string> error;
        try {
            job();
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (*alive) {
            CallAfter([alive, done, error] {
                if (*alive) done(error);
            });
        }
    }).detach();
}

void MainFrame::ShowWindow() {
    Show();
    Iconize(false);
    Raise();
#if defined(_WIN32) && defined(LOKI_HAS_SKALD)
    OfferClientUpdate(false);
#endif
}

#if defined(_WIN32) && defined(LOKI_HAS_SKALD)
void MainFrame::CheckClientUpdates(bool manual) {
    if (checking_updates_ || prepared_update_) return;
    updates_.StartOnce(6 * 60 * 60 * 1000);
    checking_updates_ = true;
    GetMenuBar()->Enable(update_menu_id_, false);
    auto result = std::make_shared<std::optional<ClientUpdate>>();
    Background([result] { *result = check_client_update(LOKI_CLIENT_VERSION); },
        [this, result, manual](std::optional<std::string> error) {
            checking_updates_ = false;
            GetMenuBar()->Enable(update_menu_id_, true);
            if (error) {
                if (manual) wxMessageBox(W(*error), "Client updates", wxOK | wxICON_WARNING, this);
                return;
            }
            available_update_ = std::move(*result);
            if (!available_update_ && manual) {
                wxMessageBox("Loki Client is up to date.", "Client updates", wxOK | wxICON_INFORMATION, this);
                return;
            }
            OfferClientUpdate(manual);
        });
}

void MainFrame::OfferClientUpdate(bool manual) {
    if (!available_update_ || checking_updates_ || prepared_update_ || !IsShown() || !IsEnabled() ||
        (!manual && available_update_->version == offered_version_)) return;
    const auto release = *available_update_;
    offered_version_ = release.version;
    checking_updates_ = true;
    const auto consent = wxMessageBox(
        W("Loki Client " + release.version + " is available. Download it and restart when current transfers finish?"),
        "Client update", wxYES_NO | wxNO_DEFAULT | wxICON_INFORMATION, this);
    if (consent != wxYES) { checking_updates_ = false; return; }
    GetMenuBar()->Enable(update_menu_id_, false);
    GetMenuBar()->SetLabel(update_menu_id_, wxS("Downloading client update…"));
    auto result = std::make_shared<std::shared_ptr<PreparedClientUpdate>>();
    Background([release, result] { *result = prepare_client_update(release); },
        [this, result](std::optional<std::string> error) {
            checking_updates_ = false;
            if (error) {
                GetMenuBar()->SetLabel(update_menu_id_, wxS("Check for client updates…"));
                GetMenuBar()->Enable(update_menu_id_, true);
                wxMessageBox(W(*error), "Client update", wxOK | wxICON_ERROR, this);
                return;
            }
            prepared_update_ = std::move(*result);
            TryInstallClientUpdate();
        });
}

void MainFrame::TryInstallClientUpdate() {
    if (!IsEnabled() || syncing_ || file_browser_busy(static_cast<wxPanel*>(pages_->GetPage(1))) ||
        workspace_page_busy(static_cast<wxPanel*>(pages_->GetPage(2))) ||
        workspace_page_busy(static_cast<wxPanel*>(pages_->GetPage(3)))) {
        GetMenuBar()->SetLabel(update_menu_id_, wxS("Update ready. Waiting for transfers…"));
        updates_.StartOnce(2000);
        return;
    }
    try {
        prepared_update_->install_after_exit();
        Quit();
    } catch (const std::exception& error) {
        prepared_update_.reset();
        GetMenuBar()->SetLabel(update_menu_id_, wxS("Check for client updates…"));
        GetMenuBar()->Enable(update_menu_id_, true);
        updates_.StartOnce(6 * 60 * 60 * 1000);
        wxMessageBox(W(error.what()), "Client update", wxOK | wxICON_ERROR, this);
    }
}
#endif

void MainFrame::Quit() {
    quitting_ = true;
    Close(true);
}

void MainFrame::BrowseFiles() {
    ShowWindow();
    if (store_.signed_in()) OpenPage(1);
}

wxPanel* MainFrame::BuildWorkspace(wxWindow* parent) {
    auto* workspace = new wxPanel(parent);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    sidebar_ = new ui::Sidebar(workspace, {std::begin(kPages), std::end(kPages)});
    sidebar_->on_select = [this](int index) { OpenPage(index); };
    sidebar_->on_account = [this](wxPoint where) { ShowAccountMenu(where); };
    row->Add(sidebar_, 0, wxEXPAND);

    pages_ = new wxSimplebook(workspace);
    pages_->AddPage(BuildFolders(pages_), kPages[0]);
    pages_->AddPage(build_file_browser(pages_, store_), kPages[1]);
    pages_->AddPage(build_updates_page(pages_, store_), kPages[2]);
    pages_->AddPage(build_repository_page(pages_, store_), kPages[3]);
    for (size_t i = 0; i < pages_->GetPageCount(); ++i) pages_->GetPage(i)->SetBackgroundColour(ui::page_background());
    row->Add(pages_, 1, wxEXPAND);
    workspace->SetSizer(row);
    return workspace;
}

void MainFrame::OpenPage(int index) {
    if (index < 0 || index > 3 || !store_.signed_in()) return;
    book_->SetSelection(1);
    pages_->SetSelection(index);
    sidebar_->Select(index);
    sidebar_->SetAccount(W(store_.display_name()), W(store_.server()));
    switch (index) {
        case 1: activate_file_browser(static_cast<wxPanel*>(pages_->GetPage(1))); break;
        case 2: activate_updates_page(static_cast<wxPanel*>(pages_->GetPage(2))); break;
        case 3: activate_repository_page(static_cast<wxPanel*>(pages_->GetPage(3))); break;
    }
    Layout();
}

// ---- Connecting -------------------------------------------------------------

wxPanel* MainFrame::BuildConnect(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    page->SetBackgroundColour(ui::page_background());
    auto* column = new wxBoxSizer(wxVERTICAL);
    column->Add(new wxStaticBitmap(page, wxID_ANY, ui::tree_bitmap(FromDIP(44), wxColour(14, 22, 18))), 0,
                wxBOTTOM, FromDIP(16));
    auto* title = new wxStaticText(page, wxID_ANY, "Connect to LOKI");
    title->SetFont(title->GetFont().Scaled(1.6f).Bold());
    column->Add(title, 0, wxBOTTOM, FromDIP(4));
    column->Add(ui::paragraph(page, "Sync emulator saves and other folders, browse shared files, and work with "
                                    "repositories on your own LOKI server.", ui::muted()),
                0, wxEXPAND | wxBOTTOM, FromDIP(20));

    column->Add(new wxStaticText(page, wxID_ANY, "Server address"), 0, wxBOTTOM, FromDIP(4));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    server_ = new wxTextCtrl(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    server_->SetHint("loki.example.com");
    continue_ = new wxButton(page, wxID_ANY, "Continue");
    row->Add(server_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row->Add(continue_, 0, wxALIGN_CENTER_VERTICAL);
    column->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    server_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { OnContinue(); });
    http_warning_ = ui::paragraph(page, kPlainHttpWarning, ui::danger());
    http_warning_->Hide();
    column->Add(http_warning_, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    server_->Bind(wxEVT_TEXT, [this, page](wxCommandEvent& e) {
        http_warning_->Show(plain_http(server_->GetValue()));
        page->Layout();
        e.Skip();
    });
    continue_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnContinue(); });

    // Sign-in choices, shown once the server answers.
    signin_ = new wxPanel(page);
    auto* signin = new wxBoxSizer(wxVERTICAL);
    signin->Add(new wxStaticLine(signin_), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    server_name_ = new wxStaticText(signin_, wxID_ANY, "");
    server_name_->SetFont(server_name_->GetFont().Scaled(1.15f).Bold());
    signin->Add(server_name_, 0, wxBOTTOM, FromDIP(6));
    signin->Add(ui::paragraph(signin_, "Pair this computer by approving a short code in the LOKI dashboard. Your "
                                       "password never touches this computer."),
                0, wxEXPAND | wxBOTTOM, FromDIP(10));
    auto* choices = new wxBoxSizer(wxHORIZONTAL);
    ui::button(signin_, choices, "Pair with a code", [this] { OnPair(); });
    back_ = ui::button(signin_, choices, "Back to saved account", [this] { if (store_.signed_in()) ShowSession(); });
    signin->Add(choices, 0, wxBOTTOM, FromDIP(18));

    auto* box = new wxStaticBoxSizer(wxVERTICAL, signin_, "Or sign in with a password");
    auto* grid = new wxFlexGridSizer(2, FromDIP(wxSize(10, 8)));
    grid->AddGrowableCol(1);
    username_ = new wxTextCtrl(box->GetStaticBox(), wxID_ANY);
    password_ = new wxTextCtrl(box->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                               wxTE_PASSWORD | wxTE_PROCESS_ENTER);
    code_ = new wxTextCtrl(box->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    code_->SetHint("Only if two-factor authentication is on");
    grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Username or email"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(username_, 1, wxEXPAND);
    grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Password"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(password_, 1, wxEXPAND);
    grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Authentication code"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(code_, 1, wxEXPAND);
    box->Add(grid, 0, wxEXPAND | wxALL, FromDIP(10));
    auto* login = new wxButton(box->GetStaticBox(), wxID_ANY, "Sign in");
    login->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnLogin(); });
    password_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { OnLogin(); });
    code_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { OnLogin(); });
    box->Add(login, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(10));
    signin->Add(box, 0, wxEXPAND);
    signin_->SetSizer(signin);
    column->Add(signin_, 0, wxEXPAND);

    // The code to approve, while pairing.
    pairing_panel_ = new wxPanel(page);
    auto* pairing = new wxBoxSizer(wxVERTICAL);
    pairing->Add(new wxStaticLine(pairing_panel_), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
    pairing->Add(ui::paragraph(pairing_panel_,
                               wxS("Approve this code in the LOKI dashboard under Account → Devices → Pair a device.")),
                 0, wxEXPAND | wxBOTTOM, FromDIP(10));
    user_code_ = new wxStaticText(pairing_panel_, wxID_ANY, "");
    user_code_->SetFont(wxFont(wxFontInfo(28).Family(wxFONTFAMILY_TELETYPE).Bold()));
    pairing->Add(user_code_, 0, wxBOTTOM, FromDIP(6));
    pairing->Add(ui::paragraph(pairing_panel_, wxS("Waiting for approval…"), ui::muted()), 0, wxEXPAND | wxBOTTOM,
                 FromDIP(12));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    ui::button(pairing_panel_, buttons, "Open approval page", [this] { wxLaunchDefaultBrowser(approval_url_); });
    ui::button(pairing_panel_, buttons, "Cancel", [this] {
        if (pairing_alive_) *pairing_alive_ = false;
        ShowConnectStep();
    });
    pairing->Add(buttons);
    pairing_panel_->SetSizer(pairing);
    column->Add(pairing_panel_, 0, wxEXPAND);

    connect_status_ = new ui::Status(page);
    column->Add(connect_status_, 0, wxEXPAND | wxTOP, FromDIP(12));

    // A readable column in the middle of the window, nearer the top than the bottom.
    auto* centred = new wxBoxSizer(wxHORIZONTAL);
    centred->AddStretchSpacer();
    centred->Add(column, 0, wxEXPAND);
    centred->AddStretchSpacer();
    column->SetMinSize(FromDIP(wxSize(520, -1)));
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->AddStretchSpacer(1);
    outer->Add(centred, 0, wxEXPAND | wxALL, FromDIP(24));
    outer->AddStretchSpacer(2);
    page->SetSizer(outer);
    signin_->Hide();
    pairing_panel_->Hide();
    return page;
}

void MainFrame::ShowConnectStep() {
    signin_->Show(discovered_.has_value());
    back_->Show(store_.signed_in());
    pairing_panel_->Hide();
    book_->GetPage(0)->Layout();
}

void MainFrame::OnContinue() {
    const auto server = U(server_->GetValue());
    if (server.empty()) return;
    continue_->Disable();
    connect_status_->Info(wxS("Contacting the server…"));
    auto name = std::make_shared<std::string>();
    auto base = std::make_shared<std::string>();
    Background(
        [server, name, base] {
            Api api(server);
            *name = api.discover().name;
            *base = api.base();
        },
        [this, name, base](std::optional<std::string> error) {
            continue_->Enable();
            connect_status_->Clear();
            if (error) {
                discovered_.reset();
                connect_status_->Error(W(*error));
            } else {
                discovered_ = *base;
                server_->SetValue(W(*base));
                server_name_->SetLabel(W(*name));
            }
            ShowConnectStep();
        });
}

void MainFrame::OnPair() {
    if (!discovered_) return;
    const auto server = *discovered_;
    auto pairing = std::make_shared<SignIn>();
    const auto directory = store_.directory();
    Background([server, pairing, directory] {
                   Store other(directory);
                   *pairing = Desktop(other).pair(server);
               },
               [this, pairing](std::optional<std::string> error) {
                   if (error) { connect_status_->Error(W(*error)); return; }
                   connect_status_->Clear();
                   user_code_->SetLabel(W(pairing->pairing.user_code));
                   approval_url_ = W(pairing->pairing.verification_uri_complete);
                   signin_->Hide();
                   pairing_panel_->Show();
                   book_->GetPage(0)->Layout();
                   if (pairing_alive_) *pairing_alive_ = false;
                   pairing_alive_ = std::make_shared<std::atomic<bool>>(true);
                   PollPairing(*pairing);
               });
}

void MainFrame::PollPairing(SignIn pairing) {
    const auto active = pairing_alive_;
    const auto directory = store_.directory();
    Background(
        [active, pairing, directory] {
            Store other(directory);
            Desktop client(other);
            if (const auto session = client.wait(pairing, [active] { return active->load(); }))
                client.remember(pairing.server, *session);
        },
        [this, active](std::optional<std::string> error) {
            if (!*active) return;
            if (error) {
                connect_status_->Error(W(*error));
                ShowConnectStep();
            } else {
                ProfileChanged();
            }
        });
}

void MainFrame::OnLogin() {
    if (!discovered_) return;
    const auto server = *discovered_;
    const auto username = U(username_->GetValue()), password = U(password_->GetValue()), code = U(code_->GetValue());
    const auto directory = store_.directory();
    password_->Clear();
    code_->Clear();
    connect_status_->Info(wxS("Signing in…"));
    Background(
        [=] { Store other(directory); Desktop(other).login(server, username, password, code); },
        [this](std::optional<std::string> error) {
            if (error) { connect_status_->Error(W(*error)); return; }
            connect_status_->Clear();
            ProfileChanged();
        });
}

void MainFrame::ShowSession() {
    pairing_panel_->Hide();
    OpenPage(store_.can_sync() ? 0 : 1);
    Refresh();
    StartWatching();
    WatchFolders();
    SyncNow();
}

void MainFrame::ProfileChanged() {
    try {
        const auto token = store_.token();
        const auto mappings = store_.mappings();
        if (token != profile_token_) {
            profile_token_ = token;
            shown_mappings_ = mappings;
            if (!token.empty()) {
                if (pairing_alive_) *pairing_alive_ = false;
                ShowSession();
            } else {
                StopWatching();
                if (files_) files_->RemoveAll();
                folders_.clear();
                list_->DeleteAllItems();
                activate_file_browser(static_cast<wxPanel*>(pages_->GetPage(1)));
                activate_updates_page(static_cast<wxPanel*>(pages_->GetPage(2)));
                activate_repository_page(static_cast<wxPanel*>(pages_->GetPage(3)));
                discovered_.reset();
                ShowConnectStep();
                book_->SetSelection(0);
            }
            return;
        }
        if (!token.empty() && shown_mappings_ != mappings) {
            bool paths_changed = shown_mappings_.size() != mappings.size();
            for (const auto& [id, mapping] : mappings) {
                const auto old = shown_mappings_.find(id);
                paths_changed |= old == shown_mappings_.end() || old->second.local != mapping.local;
            }
            shown_mappings_ = mappings;
            if (paths_changed) { WatchFolders(); SyncNow(); }
            Populate();
        }
    } catch (const std::exception& error) {
        status_->Error(W(error.what()));
    }
}

// ---- Noticing changes -------------------------------------------------------

void MainFrame::StartWatching() {
    StopWatching();
    if (!store_.signed_in() || !store_.can_sync()) return;
    auto active = watching_ = std::make_shared<std::atomic<bool>>(true);
    std::thread([this, active, alive = alive_, api = store_.api()]() mutable {
        Desktop::watch(api, [active, alive] { return *active && *alive; }, [this, active, alive] {
            wxTheApp->CallAfter([this, active, alive] {
                if (*active && *alive) SyncNow();
            });
        });
    }).detach();
}

void MainFrame::StopWatching() {
    if (watching_) *watching_ = false;
    watching_.reset();
}

void MainFrame::WatchFolders() {
    if (!files_) {
        files_ = std::make_unique<wxFileSystemWatcher>();
        files_->SetOwner(this);
    }
    files_->RemoveAll();
    for (const auto& [id, mapping] : store_.mappings()) {
        if (std::filesystem::is_directory(mapping.local)) {
            files_->AddTree(wxFileName::DirName(W(path_utf8(mapping.local))), wxFSW_EVENT_ALL);
        }
    }
}

// ---- Folders ----------------------------------------------------------------

wxPanel* MainFrame::BuildFolders(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* column = new wxBoxSizer(wxVERTICAL);

    auto header = ui::page_header(page, "Save sync");
    account_ = header.subtitle;
    ui::button(page, header.actions, wxS("New folder…"), [this] { OnNewFolder(); });
    ui::button(page, header.actions, "Sync now", [this] { Refresh(); SyncNow(); });
    column->Add(header.sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(16));

    quota_text_ = new wxStaticText(page, wxID_ANY, "");
    quota_text_->SetForegroundColour(ui::muted());
    column->Add(quota_text_, 0, wxBOTTOM, FromDIP(4));
    quota_ = new wxGauge(page, wxID_ANY, 1000, wxDefaultPosition, FromDIP(wxSize(-1, 6)));
    column->Add(quota_, 0, wxEXPAND | wxBOTTOM, FromDIP(14));

    list_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    list_->AppendColumn("Folder", wxLIST_FORMAT_LEFT, FromDIP(160));
    list_->AppendColumn("Holds", wxLIST_FORMAT_LEFT, FromDIP(150));
    list_->AppendColumn("On this computer", wxLIST_FORMAT_LEFT, FromDIP(260));
    list_->AppendColumn("Last sync", wxLIST_FORMAT_LEFT, FromDIP(200));
    ui::fill_last_column(list_);
    // wxWidgets rescales fonts and sizers; list column widths are application-owned.
    list_->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) {
        for (int i = 0; i < list_->GetColumnCount(); ++i) {
            list_->SetColumnWidth(i, e.ScaleX(list_->GetColumnWidth(i)));
        }
        e.Skip();
    });
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { OnChooseLocal(); });
    list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { UpdateFolderButtons(); });
    list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { UpdateFolderButtons(); });
    column->Add(list_, 1, wxEXPAND);

    // Actions for the selected folder.
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    choose_local_ = ui::button(page, buttons, wxS("Choose local folder…"), [this] { OnChooseLocal(); });
    stop_ = ui::button(page, buttons, "Stop syncing here", [this] { OnStop(); });
    // Shown when a sync held back deleting files that went missing here.
    restore_missing_ = ui::button(page, buttons, "Bring missing files back", [this] { OnMissing(restore_missing); });
    delete_missing_ = ui::button(page, buttons, wxS("Delete missing files everywhere…"), [this] { OnMissing(delete_missing); });
    column->Add(buttons, 0, wxTOP, FromDIP(10));

    status_ = new ui::Status(page);
    column->Add(status_, 0, wxEXPAND | wxTOP, FromDIP(10));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(column, 1, wxEXPAND | wxALL, FromDIP(24));
    page->SetSizer(outer);
    UpdateFolderButtons();
    return page;
}

void MainFrame::Refresh() {
    if (!store_.signed_in()) return;
    account_->SetLabel("Folders kept the same on every device, with earlier versions kept on the server");
    auto folders = std::make_shared<std::vector<Folder>>();
    auto quota = std::make_shared<Quota>();
    auto access = std::make_shared<bool>(true);
    auto api = std::make_shared<Api>(store_.api());
    Background(
        [api, folders, quota, access] {
            *access = api->sync_access();
            if (!*access) return;
            *folders = api->folders();
            *quota = api->quota();
        },
        [this, folders, quota, access](std::optional<std::string> error) {
            if (error) {
                status_->Error(W(*error));
                return;
            }
            store_.set_sync_access(*access);
            if (!*access) {
                StopWatching(); folders_.clear(); list_->DeleteAllItems(); quota_->SetValue(0);
                quota_text_->SetLabel("");
                status_->Info("Your account does not have access to Save sync. Shared files, apps, and "
                              "repositories are still available from the sidebar.");
                return;
            }
            if (!watching_ || !*watching_) StartWatching();
            folders_ = *folders;
            const auto limit = std::max<std::int64_t>(quota->limit, 1);
            quota_->SetValue(int(std::min<std::int64_t>(1000, quota->used * 1000 / limit)));
            quota_text_->SetLabel(size_text(quota->used) + " of " + size_text(quota->limit) +
                                  " used on the server, including every kept version");
            Populate();
        });
}

void MainFrame::Populate() {
    const auto selected = SelectedFolder();
    const auto mappings = store_.mappings();
    list_->DeleteAllItems();
    for (size_t i = 0; i < folders_.size(); ++i) {
        const auto& f = folders_[i];
        const auto row = list_->InsertItem(long(i), W(f.name));
        list_->SetItem(row, 1, kind_label(f.kind));
        auto m = mappings.find(f.id);
        if (m == mappings.end()) {
            list_->SetItem(row, 2, wxS("Not on this computer"));
            list_->SetItemTextColour(row, ui::muted());
        } else {
            list_->SetItem(row, 2, W(path_utf8(m->second.local)));
            list_->SetItem(row, 3, when(m->second.last_sync) + (m->second.last_result.empty() ? wxS("") : wxS(" · ")) +
                                       W(m->second.last_result));
            if (m->second.failed) list_->SetItemTextColour(row, ui::danger());
        }
        if (selected && *selected == f.id) list_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
    if (folders_.empty()) status_->Info("No sync folders yet. Use New folder to create one on the server.");
    UpdateFolderButtons();
}

void MainFrame::UpdateFolderButtons() {
    const auto id = SelectedFolder();
    const auto mapping = id ? store_.mapping(*id) : std::nullopt;
    const bool mapped = mapping.has_value();
    choose_local_->Enable(id.has_value());
    choose_local_->SetLabel(mapped ? wxS("Change local folder…") : wxS("Choose local folder…"));
    stop_->Enable(mapped);
    const bool held = mapped && mapping->held > 0;
    restore_missing_->Show(held);
    delete_missing_->Show(held);
    choose_local_->GetParent()->Layout();
}

std::optional<std::string> MainFrame::SelectedFolder() const {
    const auto row = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || size_t(row) >= folders_.size()) return std::nullopt;
    return folders_[size_t(row)].id;
}

void MainFrame::OnChooseLocal() {
    const auto id = SelectedFolder();
    if (!id) {
        status_->Info("Select a folder first.");
        return;
    }
    const auto current = store_.mapping(*id);
    wxDirDialog dialog(this, "Choose where this folder lives on this computer",
                       current ? W(path_utf8(current->local)) : wxString());
    if (dialog.ShowModal() != wxID_OK) return;
    const auto chosen = path_from_utf8(U(dialog.GetPath()));
    try { Desktop(store_).map(*id, chosen); }
    catch (const std::exception& error) { status_->Error(W(error.what())); return; }
    shown_mappings_ = store_.mappings();
    WatchFolders();
    Populate();
    SyncNow();
}

void MainFrame::OnStop() {
    if (const auto id = SelectedFolder()) {
        const auto name = list_->GetItemText(list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED));
        if (wxMessageBox("Stop syncing " + name + " on this computer? The local files stay where they are, and the "
                         "server keeps its copy.", "Stop syncing", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
                         this) != wxYES)
            return;
        Desktop(store_).unmap(*id);
        shown_mappings_ = store_.mappings();
        WatchFolders();
        Populate();
    }
}

void MainFrame::OnMissing(const char* decision) {
    const auto id = SelectedFolder();
    const auto mapping = id ? store_.mapping(*id) : std::nullopt;
    if (!mapping || mapping->held == 0) return;
    if (std::string_view(decision) == delete_missing &&
        wxMessageBox(wxString::Format(wxS("Delete %d missing files on every device? The server keeps them in this "
                                          "folder's history, where you can restore them or roll the folder back "
                                          "from the LOKI dashboard."),
                                      mapping->held),
                     "Delete everywhere", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) {
        return;
    }
    store_.decide(*id, decision);
    SyncNow();
}

void MainFrame::OnNewFolder() {
    wxTextEntryDialog name(this, "Name for the new folder on the server", "New sync folder");
    if (name.ShowModal() != wxID_OK || name.GetValue().Trim().IsEmpty()) return;
    wxArrayString labels;
    for (const auto& [value, label] : kinds) labels.Add(label);
    wxSingleChoiceDialog kind(this, "What will it hold?", "New sync folder", labels);
    if (kind.ShowModal() != wxID_OK) return;
    const std::string folder_name = U(name.GetValue().Trim());
    const std::string folder_kind = kinds[kind.GetSelection()].first;
    auto api = std::make_shared<Api>(store_.api());
    Background([api, folder_name, folder_kind] { api->create_folder(folder_name, folder_kind); },
               [this](std::optional<std::string> error) {
                   if (error) status_->Error(W(*error));
                   Refresh();
               });
}

void MainFrame::OnSignOut() {
    if (wxMessageBox("Sign out this computer? Files stay where they are but stop syncing.", "Sign out",
                     wxYES_NO | wxICON_QUESTION, this) != wxYES) {
        return;
    }
    const auto directory = store_.directory();
    Background([directory] { Store other(directory); Desktop(other).logout(); },
        [this](std::optional<std::string> error) {
            if (error && wxMessageBox(W(*error + "\n\nClear the local sign-in anyway?"),
                                      "Sign out", wxYES_NO | wxICON_QUESTION, this) == wxYES) {
                Desktop(store_).logout(true);
            }
            ProfileChanged();
        });
}

void MainFrame::SyncNow() {
#if defined(_WIN32) && defined(LOKI_HAS_SKALD)
    if (prepared_update_) return;
#endif
    if (!store_.signed_in() || !store_.can_sync()) return;
    if (syncing_.exchange(true)) {
        rerun_ = true;
        return;
    }
    rerun_ = false;
    status_->Info(wxS("Syncing…"));
    if (tray_) tray_->SetIcon(tree_icon(32), wxS("LOKI Client — syncing"));
    auto outcomes = std::make_shared<std::map<std::string, Outcome>>();
    Background([directory = store_.directory(), outcomes] {
                   Store other(directory);
                   *outcomes = Desktop(other).sync();
               },
               [this, outcomes](std::optional<std::string> error) {
                   syncing_ = false;
                   int failed = 0, held = 0;
                   for (const auto& [id, outcome] : *outcomes) {
                       failed += outcome.errors.empty() ? 0 : 1;
                       held += outcome.held;
                   }
                   if (error) status_->Error(W(*error));
                   else if (held) {
                       const auto text = wxString::Format(
                           wxS("%d synced files are missing on this computer. Nothing was deleted on the server; select "
                               "the folder to bring them back or delete them everywhere."),
                           held);
                       status_->Error(text);
                       if (tray_ && !IsShown()) tray_->ShowBalloon("Files missing from a synced folder", text);
                   }
                   else if (failed)
                       status_->Error(wxString::Format(failed == 1 ? wxS("Synced at %s. 1 folder needs attention.")
                                                                   : wxS("Synced at %s. %d folders need attention."),
                                                       wxDateTime::Now().FormatTime(), failed));
                   else status_->Info("Synced at " + wxDateTime::Now().FormatTime());
                   if (tray_) tray_->SetIcon(tree_icon(32), "LOKI Client");
                   Populate();
                   if (rerun_) CallAfter([this] { SyncNow(); });
               });
}

}  // namespace loki
