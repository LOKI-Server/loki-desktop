#include "main_frame.hpp"

#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/iconbndl.h>
#include <wx/numdlg.h>
#include <wx/platinfo.h>
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

/** A small world-tree glyph in LOKI green, drawn rather than shipped as a file. */
wxIcon tree_icon(int size) {
    wxBitmap bitmap(size, size, 32);
    bitmap.UseAlpha();
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(wxColour(8, 14, 11)));
        dc.Clear();
        const double s = size / 32.0;
        dc.SetPen(wxPen(wxColour(125, 231, 167), std::max(2, int(3 * s)), wxPENSTYLE_SOLID));
        auto line = [&](double x1, double y1, double x2, double y2) {
            dc.DrawLine(int(x1 * s), int(y1 * s), int(x2 * s), int(y2 * s));
        };
        line(16, 27, 16, 6);
        line(16, 13, 9, 7);
        line(16, 13, 23, 7);
        line(16, 20, 10, 15);
        line(16, 20, 22, 15);
        line(11, 27, 21, 27);
    }
    wxIcon icon;
    icon.CopyFromBitmap(bitmap);
    return icon;
}

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
        const auto browse = wxWindow::NewControlId();
        menu->Append(browse, wxS("Browse files…"));
        menu->AppendSeparator();
        menu->Append(wxID_EXIT, "Quit");
        menu->Bind(wxEVT_MENU, [this, browse](wxCommandEvent& e) {
            if (e.GetId() == wxID_OPEN) frame_->ShowWindow();
            if (e.GetId() == wxID_REFRESH) frame_->SyncNow();
            if (e.GetId() == browse) frame_->BrowseFiles();
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
    auto* menus = new wxMenuBar;
    auto* app_menu = new wxMenu;
    const auto browse = wxWindow::NewControlId();
    app_menu->Append(browse, wxS("Browse files…"));
    const auto commands = wxWindow::NewControlId();
    app_menu->Append(commands, wxS("Git repositories"));
    const auto connect = wxWindow::NewControlId();
    app_menu->Append(connect, wxS("Reconnect to LOKI…"));
    app_menu->Append(wxID_REFRESH, "Sync now");
#ifdef __WXMSW__
    const auto autostart = wxWindow::NewControlId();
    app_menu->AppendCheckItem(autostart, "Start at sign-in");
    // Keep the entry pointing at this copy, in case the app was moved since it was set.

    app_menu->Check(autostart, starts_at_sign_in());
    Bind(wxEVT_MENU, [this, autostart](wxCommandEvent& e) {
        try { set_start_at_sign_in(e.IsChecked()); }
        catch (const std::exception& error) { wxMessageBox(W(error.what()), "Start at sign-in", wxICON_ERROR, this); }
        GetMenuBar()->Check(autostart, starts_at_sign_in());
    }, autostart);
#endif
    app_menu->AppendSeparator();
    app_menu->Append(wxID_EXIT, "Quit");
    menus->Append(app_menu, "&More");
    SetMenuBar(menus);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { BrowseFiles(); }, browse);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (store_.signed_in()) OpenPage(3); }, commands);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) {
        if (!store_.signed_in()) return;
        if (pairing_alive_) *pairing_alive_ = false;
        server_->SetValue(W(store_.server()));
        discovered_ = store_.server();
        ShowConnectStep();
        book_->SetSelection(0);
    }, connect);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { SyncNow(); }, wxID_REFRESH);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Quit(); }, wxID_EXIT);
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
        CallAfter([this] { WatchFolders(); OpenPage(1); });
    } else {
        book_->SetSelection(0);
        server_->SetValue(W(store_.server()));
    }
    Centre();
}

MainFrame::~MainFrame() {
    *alive_ = false;
    if (pairing_alive_) *pairing_alive_ = false;
    StopWatching();
    timer_.Stop();
    settle_.Stop();
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
}

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
    auto* sidebar = new wxPanel(workspace);
    sidebar->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
    auto* column = new wxBoxSizer(wxVERTICAL);
    auto* brand = new wxStaticText(sidebar, wxID_ANY, "LOKI");
    brand->SetFont(brand->GetFont().Scaled(1.55f).Bold());
    column->Add(brand, 0, wxALL, FromDIP(14));
    navigation_ = new wxListBox(sidebar, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(146, 132)),
        {"Sync", "Browse", "Apps", "Git"}, wxLB_SINGLE | wxBORDER_NONE);
    navigation_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent& event) { OpenPage(event.GetSelection()); });
    column->Add(navigation_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    column->AddStretchSpacer();
    profile_label_ = new wxStaticText(sidebar, wxID_ANY, "");
    column->Add(profile_label_, 0, wxEXPAND | wxALL, FromDIP(12));
    auto* sign_out = new wxButton(sidebar, wxID_ANY, "Sign out");
    sign_out->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSignOut(); });
    column->Add(sign_out, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    sidebar->SetSizer(column);
    row->Add(sidebar, 0, wxEXPAND);

    pages_ = new wxSimplebook(workspace);
    pages_->AddPage(BuildFolders(pages_), "Sync");
    pages_->AddPage(build_file_browser(pages_, store_), "Browse");
    pages_->AddPage(build_apps_page(pages_, store_), "Apps");
    pages_->AddPage(build_git_page(pages_, store_), "Git");
    row->Add(pages_, 1, wxEXPAND);
    workspace->SetSizer(row);
    return workspace;
}

void MainFrame::OpenPage(int index) {
    if (index < 0 || index > 3 || !store_.signed_in()) return;
    book_->SetSelection(1);
    pages_->SetSelection(index);
    navigation_->SetSelection(index);
    profile_label_->SetLabel(W(store_.display_name()));
    switch (index) {
        case 1: activate_file_browser(static_cast<wxPanel*>(pages_->GetPage(1))); break;
        case 2: activate_apps_page(static_cast<wxPanel*>(pages_->GetPage(2))); break;
        case 3: activate_git_page(static_cast<wxPanel*>(pages_->GetPage(3))); break;
    }
    Layout();
}

// ---- Connecting -------------------------------------------------------------

wxPanel* MainFrame::BuildConnect(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* column = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(page, wxID_ANY, "Connect to LOKI");
    title->SetFont(title->GetFont().Scaled(1.6f).Bold());
    column->Add(title, 0, wxBOTTOM, FromDIP(6));
    column->Add(new wxStaticText(page, wxID_ANY,
                                 "Keep emulator saves and other folders the same on every device, with every earlier "
                                 "version kept on your server."),
                0, wxBOTTOM, FromDIP(16));

    column->Add(new wxStaticText(page, wxID_ANY, "Server address"));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    server_ = new wxTextCtrl(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    server_->SetHint("loki.example.com");
    continue_ = new wxButton(page, wxID_ANY, "Continue");
    row->Add(server_, 1, wxRIGHT, FromDIP(8));
    row->Add(continue_);
    column->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(16));
    server_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { OnContinue(); });
    http_warning_ = new wxStaticText(page, wxID_ANY, kPlainHttpWarning);
    http_warning_->SetForegroundColour(wxColour(200, 70, 60));
    http_warning_->Wrap(FromDIP(620));
    http_warning_->Hide();
    column->Add(http_warning_, 0, wxBOTTOM, FromDIP(12));
    server_->Bind(wxEVT_TEXT, [this, page](wxCommandEvent& e) {
        http_warning_->Show(plain_http(server_->GetValue()));
        page->Layout();
        e.Skip();
    });
    continue_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnContinue(); });

    // Sign-in choices, shown once the server answers.
    signin_ = new wxPanel(page);
    auto* signin = new wxBoxSizer(wxVERTICAL);
    server_name_ = new wxStaticText(signin_, wxID_ANY, "");
    server_name_->SetFont(server_name_->GetFont().Bold());
    signin->Add(server_name_, 0, wxBOTTOM, FromDIP(8));
    signin->Add(new wxStaticText(signin_, wxID_ANY,
                                 "Pair this computer by approving a short code in the LOKI dashboard. Your password "
                                 "never touches this computer."),
                0, wxBOTTOM, FromDIP(8));
    auto* back = new wxButton(signin_, wxID_ANY, "Back to saved account");
    back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (store_.signed_in()) ShowSession(); });
    signin->Add(back, 0, wxBOTTOM, FromDIP(10));
    auto* pair = new wxButton(signin_, wxID_ANY, "Pair with a code");
    pair->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnPair(); });
    signin->Add(pair, 0, wxBOTTOM, FromDIP(16));

    auto* box = new wxStaticBoxSizer(wxVERTICAL, signin_, "Or sign in with a password");
    auto* grid = new wxFlexGridSizer(2, FromDIP(wxSize(8, 6)));
    grid->AddGrowableCol(1);
    username_ = new wxTextCtrl(box->GetStaticBox(), wxID_ANY);
    password_ = new wxTextCtrl(box->GetStaticBox(), wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    code_ = new wxTextCtrl(box->GetStaticBox(), wxID_ANY);
    code_->SetHint("only if two-factor authentication is on");
    grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Username or email"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(username_, 1, wxEXPAND);
    grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Password"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(password_, 1, wxEXPAND);
    grid->Add(new wxStaticText(box->GetStaticBox(), wxID_ANY, "Authentication code"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(code_, 1, wxEXPAND);
    box->Add(grid, 0, wxEXPAND | wxALL, FromDIP(8));
    auto* login = new wxButton(box->GetStaticBox(), wxID_ANY, "Sign in");
    login->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnLogin(); });
    box->Add(login, 0, wxLEFT | wxBOTTOM, FromDIP(8));
    signin->Add(box, 0, wxEXPAND);
    signin_->SetSizer(signin);
    column->Add(signin_, 0, wxEXPAND);

    // The code to approve, while pairing.
    pairing_panel_ = new wxPanel(page);
    auto* pairing = new wxBoxSizer(wxVERTICAL);
    pairing->Add(new wxStaticText(pairing_panel_, wxID_ANY,
                                  wxS("Approve this code in the LOKI dashboard: Account → Devices → Pair a device.")),
                 0, wxBOTTOM, FromDIP(8));
    user_code_ = new wxStaticText(pairing_panel_, wxID_ANY, "");
    user_code_->SetFont(wxFont(wxFontInfo(28).Family(wxFONTFAMILY_TELETYPE).Bold()));
    pairing->Add(user_code_, 0, wxBOTTOM, FromDIP(8));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* open = new wxButton(pairing_panel_, wxID_ANY, "Open approval page");
    open->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { wxLaunchDefaultBrowser(approval_url_); });
    auto* cancel = new wxButton(pairing_panel_, wxID_ANY, "Cancel");
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (pairing_alive_) *pairing_alive_ = false;
        ShowConnectStep();
    });
    buttons->Add(open, 0, wxRIGHT, FromDIP(8));
    buttons->Add(cancel);
    pairing->Add(buttons);
    pairing_panel_->SetSizer(pairing);
    column->Add(pairing_panel_, 0, wxEXPAND);

    connect_status_ = new wxStaticText(page, wxID_ANY, "");
    connect_status_->SetForegroundColour(wxColour(200, 70, 60));
    column->Add(connect_status_, 0, wxTOP, FromDIP(12));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(column, 1, wxEXPAND | wxALL, FromDIP(24));
    page->SetSizer(outer);
    signin_->Hide();
    pairing_panel_->Hide();
    return page;
}

void MainFrame::ShowConnectStep() {
    signin_->Show(discovered_.has_value());
    pairing_panel_->Hide();
    book_->GetPage(0)->Layout();
}

void MainFrame::OnContinue() {
    const auto server = U(server_->GetValue());
    if (server.empty()) return;
    continue_->Disable();
    connect_status_->SetLabel("");
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
            if (error) {
                discovered_.reset();
                connect_status_->SetLabel(W(*error));
            } else {
                discovered_ = *base;
                server_->SetValue(W(*base));
                server_name_->SetLabel(W(*name + "  ·  " + *base));
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
                   if (error) { connect_status_->SetLabel(W(*error)); return; }
                   connect_status_->SetLabel("");
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
                connect_status_->SetLabel(W(*error));
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
    Background(
        [=] { Store other(directory); Desktop(other).login(server, username, password, code); },
        [this](std::optional<std::string> error) {
            if (error) { connect_status_->SetLabel(W(*error)); return; }
            ProfileChanged();
        });
}

void MainFrame::ShowSession() {
    pairing_panel_->Hide();
    OpenPage(1);
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
                activate_apps_page(static_cast<wxPanel*>(pages_->GetPage(2)));
                activate_git_page(static_cast<wxPanel*>(pages_->GetPage(3)));
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
        status_->SetLabel(W(error.what()));
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

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* titles = new wxBoxSizer(wxVERTICAL);
    auto* title = new wxStaticText(page, wxID_ANY, "Save sync");
    title->SetFont(title->GetFont().Scaled(1.6f).Bold());
    account_ = new wxStaticText(page, wxID_ANY, "");
    titles->Add(title);
    titles->Add(account_);
    header->Add(titles, 1, wxALIGN_CENTER_VERTICAL);
    column->Add(header, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

    quota_ = new wxGauge(page, wxID_ANY, 1000, wxDefaultPosition, FromDIP(wxSize(-1, 8)));
    quota_text_ = new wxStaticText(page, wxID_ANY, "");
    column->Add(quota_, 0, wxEXPAND);
    column->Add(quota_text_, 0, wxTOP | wxBOTTOM, FromDIP(4));

    list_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
    list_->AppendColumn("Folder", wxLIST_FORMAT_LEFT, FromDIP(150));
    list_->AppendColumn("Holds", wxLIST_FORMAT_LEFT, FromDIP(140));
    list_->AppendColumn("On this computer", wxLIST_FORMAT_LEFT, FromDIP(230));
    list_->AppendColumn("Last sync", wxLIST_FORMAT_LEFT, FromDIP(260));
    // wxWidgets rescales fonts and sizers; list column widths are application-owned.
    list_->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) {
        for (int i = 0; i < list_->GetColumnCount(); ++i) {
            list_->SetColumnWidth(i, e.ScaleX(list_->GetColumnWidth(i)));
        }
        e.Skip();
    });
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) { OnChooseLocal(); });
    column->Add(list_, 1, wxEXPAND | wxTOP, FromDIP(8));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto add = [&](const wxString& label, std::function<void()> action) {
        auto* b = new wxButton(page, wxID_ANY, label);
        b->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        buttons->Add(b, 0, wxRIGHT, FromDIP(6));
    };
    add(wxS("Choose local folder…"), [this] { OnChooseLocal(); });
    add("Stop syncing", [this] { OnStop(); });
    add("Sync now", [this] { SyncNow(); });
    add(wxS("New server folder…"), [this] { OnNewFolder(); });
    add("Refresh", [this] { Refresh(); });
    column->Add(buttons, 0, wxTOP, FromDIP(8));

    status_ = new wxStaticText(page, wxID_ANY, "");
    column->Add(status_, 0, wxTOP, FromDIP(8));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(column, 1, wxEXPAND | wxALL, FromDIP(16));
    page->SetSizer(outer);
    return page;
}

void MainFrame::Refresh() {
    if (!store_.signed_in()) return;
    account_->SetLabel(W(store_.display_name() + "  ·  " + store_.server()));
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
                status_->SetLabel(W(*error));
                return;
            }
            store_.set_sync_access(*access);
            if (!*access) {
                StopWatching(); folders_.clear(); list_->DeleteAllItems(); quota_->SetValue(0);
                quota_text_->SetLabel(wxS("File sharing is available from More → Browse files."));
                status_->SetLabel("Your account does not have access to Save sync.");
                return;
            }
            if (!watching_ || !*watching_) StartWatching();
            folders_ = *folders;
            const auto limit = std::max<std::int64_t>(quota->limit, 1);
            quota_->SetValue(int(std::min<std::int64_t>(1000, quota->used * 1000 / limit)));
            quota_text_->SetLabel(size_text(quota->used) + " of " + size_text(quota->limit) +
                                  " used on the server, including history");
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
            list_->SetItem(row, 2, wxS("Not synced here — choose a folder"));
            list_->SetItem(row, 3, "");
        } else {
            list_->SetItem(row, 2, W(path_utf8(m->second.local)));
            list_->SetItem(row, 3, when(m->second.last_sync) + (m->second.last_result.empty() ? wxS("") : wxS(" · ")) +
                                       W(m->second.last_result));
            if (m->second.failed) list_->SetItemTextColour(row, wxColour(200, 70, 60));
        }
        if (selected && *selected == f.id) list_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
    if (folders_.empty()) status_->SetLabel("No sync folders yet. Create one with New server folder.");
}

std::optional<std::string> MainFrame::SelectedFolder() const {
    const auto row = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || size_t(row) >= folders_.size()) return std::nullopt;
    return folders_[size_t(row)].id;
}

void MainFrame::OnChooseLocal() {
    const auto id = SelectedFolder();
    if (!id) {
        status_->SetLabel("Select a folder first.");
        return;
    }
    const auto current = store_.mapping(*id);
    wxDirDialog dialog(this, "Choose where this folder lives on this computer",
                       current ? W(path_utf8(current->local)) : wxString());
    if (dialog.ShowModal() != wxID_OK) return;
    const auto chosen = path_from_utf8(U(dialog.GetPath()));
    try { Desktop(store_).map(*id, chosen); }
    catch (const std::exception& error) { status_->SetLabel(W(error.what())); return; }
    shown_mappings_ = store_.mappings();
    WatchFolders();
    Populate();
    SyncNow();
}

void MainFrame::OnStop() {
    if (const auto id = SelectedFolder()) {
        Desktop(store_).unmap(*id);
        shown_mappings_ = store_.mappings();
        WatchFolders();
        Populate();
    }
}

void MainFrame::OnNewFolder() {
    wxTextEntryDialog name(this, "Name for the new server folder", "New server folder");
    if (name.ShowModal() != wxID_OK || name.GetValue().Trim().IsEmpty()) return;
    wxArrayString labels;
    for (const auto& [value, label] : kinds) labels.Add(label);
    wxSingleChoiceDialog kind(this, "What will it hold?", "New server folder", labels);
    if (kind.ShowModal() != wxID_OK) return;
    const std::string folder_name = U(name.GetValue().Trim());
    const std::string folder_kind = kinds[kind.GetSelection()].first;
    auto api = std::make_shared<Api>(store_.api());
    Background([api, folder_name, folder_kind] { api->create_folder(folder_name, folder_kind); },
               [this](std::optional<std::string> error) {
                   if (error) status_->SetLabel(W(*error));
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
    if (!store_.signed_in() || !store_.can_sync()) return;
    if (syncing_.exchange(true)) {
        rerun_ = true;
        return;
    }
    rerun_ = false;
    status_->SetLabel(wxS("Syncing…"));
    if (tray_) tray_->SetIcon(tree_icon(32), wxS("LOKI Client — syncing"));
    auto outcomes = std::make_shared<std::map<std::string, Outcome>>();
    Background([directory = store_.directory(), outcomes] {
                   Store other(directory);
                   *outcomes = Desktop(other).sync();
               },
               [this, outcomes](std::optional<std::string> error) {
                   syncing_ = false;
                   int failed = 0;
                   for (const auto& [id, outcome] : *outcomes) failed += outcome.errors.empty() ? 0 : 1;
                   status_->SetLabel(error ? W(*error)
                                     : failed ? wxString::Format(wxS("Last sync %s — %d folder(s) need attention"),
                                                                 wxDateTime::Now().FormatTime(), failed)
                                              : "Last sync " + wxDateTime::Now().FormatTime());
                   if (tray_) tray_->SetIcon(tree_icon(32), "LOKI Client");
                   Populate();
                   if (rerun_) CallAfter([this] { SyncNow(); });
               });
}

}  // namespace loki
