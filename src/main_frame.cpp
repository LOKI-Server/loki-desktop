#include "main_frame.hpp"

#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/iconbndl.h>
#include <wx/numdlg.h>
#include <wx/platinfo.h>
#include <wx/utils.h>

#include <chrono>
#include <thread>

#include "engine.hpp"

namespace loki {
namespace {

wxString W(const std::string& utf8) { return wxString::FromUTF8(utf8); }
std::string U(const wxString& text) { return text.utf8_string(); }

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string device_name() { return U(wxGetHostName()).empty() ? "Desktop" : U(wxGetHostName()); }
std::string platform() { return U(wxPlatformInfo::Get().GetOperatingSystemFamilyName().Lower()); }

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
        menu->Append(wxID_OPEN, "Open LOKI Sync");
        menu->Append(wxID_REFRESH, "Sync now");
        menu->AppendSeparator();
        menu->Append(wxID_EXIT, "Quit");
        menu->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
            if (e.GetId() == wxID_OPEN) frame_->ShowWindow();
            if (e.GetId() == wxID_REFRESH) frame_->SyncNow();
            if (e.GetId() == wxID_EXIT) frame_->Quit();
        });
        return menu;
    }

private:
    MainFrame* frame_;
};

}  // namespace

MainFrame::MainFrame(Store& store)
    : wxFrame(nullptr, wxID_ANY, "LOKI Sync"), store_(store), timer_(this), settle_(this) {
    SetSize(FromDIP(wxSize(820, 560)));
    wxIconBundle icons;
    for (const int size : {16, 24, 32, 48, 64}) icons.AddIcon(tree_icon(size));
    SetIcons(icons);
    SetMinSize(FromDIP(wxSize(640, 420)));
    book_ = new wxSimplebook(this);
    book_->AddPage(BuildConnect(book_), "Connect");
    book_->AddPage(BuildFolders(book_), "Folders");
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(book_, 1, wxEXPAND);
    SetSizer(sizer);

    if (wxTaskBarIcon::IsAvailable()) {
        tray_ = std::make_unique<Tray>(this);
        tray_->SetIcon(tree_icon(32), "LOKI Sync");
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
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { SyncNow(); });
    timer_.Start(5 * 60 * 1000);
    Bind(wxEVT_FSWATCHER, [this](wxFileSystemWatcherEvent& e) {
        if (e.GetChangeType() & (wxFSW_EVENT_ACCESS | wxFSW_EVENT_WARNING | wxFSW_EVENT_ERROR)) return;
        if (e.GetPath().GetFullName().StartsWith(".loki")) return;
        settle_.StartOnce(int(Engine::settle_ms) + 500);
    });

    if (store_.signed_in()) {
        book_->SetSelection(1);
        Refresh();
        SyncNow();
        StartWatching();
        // The file watcher needs a running event loop.
        CallAfter([this] { WatchFolders(); });
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
                                  wxS("Approve this code in the LOKI dashboard: Save sync → Pair a device.")),
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
    auto pairing = std::make_shared<Pairing>();
    Background([server, pairing] { *pairing = Api(server).pair(device_name(), platform()); },
               [this, pairing](std::optional<std::string> error) {
                   if (error) {
                       connect_status_->SetLabel(W(*error));
                       return;
                   }
                   connect_status_->SetLabel("");
                   user_code_->SetLabel(W(pairing->user_code));
                   approval_url_ = W(pairing->verification_uri_complete);
                   signin_->Hide();
                   pairing_panel_->Show();
                   book_->GetPage(0)->Layout();
                   if (pairing_alive_) *pairing_alive_ = false;
                   pairing_alive_ = std::make_shared<std::atomic<bool>>(true);
                   PollPairing(*pairing, now_ms() + pairing->expires_in * 1000LL);
               });
}

void MainFrame::PollPairing(Pairing pairing, long long deadline) {
    auto active = pairing_alive_;
    const auto server = *discovered_;
    auto session = std::make_shared<std::optional<Session>>();
    Background(
        [active, pairing, server, session] {
            std::this_thread::sleep_for(std::chrono::seconds(pairing.interval));
            if (*active) *session = Api(server).poll(pairing.device_code);
        },
        [this, active, pairing, deadline, server, session](std::optional<std::string> error) {
            if (!*active) return;
            if (error) {
                connect_status_->SetLabel(W(*error));
                ShowConnectStep();
            } else if (*session) {
                FinishSignIn(server, **session);
            } else if (now_ms() > deadline) {
                connect_status_->SetLabel("The code expired. Start pairing again.");
                ShowConnectStep();
            } else {
                PollPairing(pairing, deadline);
            }
        });
}

void MainFrame::OnLogin() {
    if (!discovered_) return;
    const auto server = *discovered_;
    const auto username = U(username_->GetValue()), password = U(password_->GetValue()), code = U(code_->GetValue());
    auto session = std::make_shared<Session>();
    Background(
        [=] { *session = Api(server).login(username, password, code, device_name(), platform()); },
        [this, server, session](std::optional<std::string> error) {
            if (error) {
                connect_status_->SetLabel(W(*error));
                return;
            }
            password_->Clear();
            FinishSignIn(server, *session);
        });
}

void MainFrame::FinishSignIn(const std::string& server, const Session& session) {
    store_.sign_in(server, session);
    pairing_panel_->Hide();
    book_->SetSelection(1);
    Refresh();
    StartWatching();
    WatchFolders();
}

// ---- Noticing changes -------------------------------------------------------

void MainFrame::StartWatching() {
    StopWatching();
    if (!store_.signed_in()) return;
    auto active = watching_ = std::make_shared<std::atomic<bool>>(true);
    std::thread([this, active, alive = alive_, api = store_.api()]() mutable {
        std::string state;
        while (*active && *alive) {
            try {
                const bool first = state.empty();
                const auto result = api.watch(state, 60);
                state = result.state;
                // The first answer only learns the state; the startup sync covered it.
                if (result.changed && !first && *active && *alive) {
                    CallAfter([this, active, alive] {
                        if (*active && *alive) SyncNow();
                    });
                }
            } catch (const std::exception&) {
                // Offline or signed out: try again shortly.
                for (int i = 0; i < 15 && *active && *alive; ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
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
            files_->AddTree(wxFileName::DirName(wxString(mapping.local.wstring())), wxFSW_EVENT_ALL);
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
    auto* sign_out = new wxButton(page, wxID_ANY, "Sign out");
    sign_out->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSignOut(); });
    header->Add(sign_out, 0, wxALIGN_CENTER_VERTICAL);
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
    auto api = std::make_shared<Api>(store_.api());
    Background(
        [api, folders, quota] {
            *folders = api->folders();
            *quota = api->quota();
        },
        [this, folders, quota](std::optional<std::string> error) {
            if (error) {
                status_->SetLabel(W(*error));
                return;
            }
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
            list_->SetItem(row, 2, W(U(wxString(m->second.local.wstring()))));
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
                       current ? wxString(current->local.wstring()) : wxString());
    if (dialog.ShowModal() != wxID_OK) return;
    const std::filesystem::path chosen = dialog.GetPath().ToStdWstring();
    if (current && current->local != chosen) store_.remove_mapping(*id);  // A new place starts a fresh index.
    store_.save_mapping({*id, chosen});
    WatchFolders();
    Populate();
    SyncNow();
}

void MainFrame::OnStop() {
    if (const auto id = SelectedFolder()) {
        store_.remove_mapping(*id);
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
    auto api = std::make_shared<Api>(store_.api());
    Background([api] { api->sign_out(); }, [](std::optional<std::string>) {});
    StopWatching();
    store_.sign_out();
    if (files_) files_->RemoveAll();
    folders_.clear();
    list_->DeleteAllItems();
    discovered_.reset();
    ShowConnectStep();
    book_->SetSelection(0);
}

void MainFrame::SyncNow() {
    if (!store_.signed_in()) return;
    if (syncing_.exchange(true)) {
        rerun_ = true;
        return;
    }
    rerun_ = false;
    status_->SetLabel(wxS("Syncing…"));
    if (tray_) tray_->SetIcon(tree_icon(32), wxS("LOKI Sync — syncing"));
    auto outcomes = std::make_shared<std::map<std::string, Outcome>>();
    Background([this, outcomes] { *outcomes = Engine(store_).sync_all(); },
               [this, outcomes](std::optional<std::string> error) {
                   syncing_ = false;
                   int failed = 0;
                   for (const auto& [id, outcome] : *outcomes) failed += outcome.errors.empty() ? 0 : 1;
                   status_->SetLabel(error ? W(*error)
                                     : failed ? wxString::Format(wxS("Last sync %s — %d folder(s) need attention"),
                                                                 wxDateTime::Now().FormatTime(), failed)
                                              : "Last sync " + wxDateTime::Now().FormatTime());
                   if (tray_) tray_->SetIcon(tree_icon(32), "LOKI Sync");
                   Populate();
                   if (rerun_) CallAfter([this] { SyncNow(); });
               });
}

}  // namespace loki
