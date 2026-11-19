#include "workspace_pages.hpp"

#include <wx/wx.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/utils.h>

#include "apps_api.hpp"
#include "commands.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace loki {
namespace {

wxString W(const std::string& text) { return wxString::FromUTF8(text); }
std::string U(const wxString& text) { return text.utf8_string(); }

class AsyncPage : public wxPanel {
public:
    AsyncPage(wxWindow* parent, Store& store, const char* title) : wxPanel(parent), store_(store) {
        layout_ = new wxBoxSizer(wxVERTICAL);
        auto* heading = new wxStaticText(this, wxID_ANY, title);
        heading->SetFont(heading->GetFont().Scaled(1.6f).Bold());
        layout_->Add(heading, 0, wxBOTTOM, FromDIP(12));
    }
    ~AsyncPage() override { *alive_ = false; }

    void Activate() {
        const auto token = store_.token();
        if (token == session_) return;
        session_ = token;
        Reset();
        refresh_queued_ = busy_ && !token.empty();
        if (!busy_ && !token.empty()) RefreshData();
    }

protected:
    virtual void Reset() = 0;
    virtual void RefreshData() = 0;
    virtual void Controls() = 0;

    void FinishLayout() {
        status_ = new wxStaticText(this, wxID_ANY, "");
        layout_->Add(status_, 0, wxEXPAND | wxTOP, FromDIP(8));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(layout_, 1, wxEXPAND | wxALL, FromDIP(16));
        SetSizer(outer);
    }

    wxButton* Button(wxBoxSizer* row, const wxString& title, const std::function<void()>& action) {
        auto* button = new wxButton(this, wxID_ANY, title);
        button->Bind(wxEVT_BUTTON, [action](wxCommandEvent&) { action(); });
        row->Add(button, 0, wxRIGHT, FromDIP(6));
        return button;
    }

    void Work(const wxString& working, std::function<void()> job, std::function<void()> done) {
        if (busy_) return;
        busy_ = true;
        status_->SetLabel(working);
        Controls();
        const auto alive = alive_;
        const auto session = session_;
        auto* application = wxTheApp;
        std::thread([this, alive, session, application, job = std::move(job), done = std::move(done)] {
            std::string error;
            try { job(); }
            catch (const std::exception& e) { error = e.what(); }
            if (!*alive) return;
            application->CallAfter([this, alive, session, done, error] {
                if (!*alive) return;
                busy_ = false;
                if (session != session_ || session != store_.token()) {
                    if (refresh_queued_) { refresh_queued_ = false; RefreshData(); }
                    return;
                }
                status_->SetLabel(W(error));
                if (error.empty()) {
                    try { done(); }
                    catch (const std::exception& e) { status_->SetLabel(W(e.what())); }
                }
                status_->Wrap(std::max(200, GetClientSize().GetWidth() - FromDIP(32)));
                Controls();
                Layout();
            });
        }).detach();
    }

    Store& store_;
    wxBoxSizer* layout_;
    wxStaticText* status_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    bool busy_ = false;
private:
    std::string session_;
    bool refresh_queued_ = false;
};

class AppsPage : public AsyncPage {
public:
    AppsPage(wxWindow* parent, Store& store) : AsyncPage(parent, store, "Apps") {
        auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
        refresh_ = Button(toolbar, "Check for updates", [this] { RefreshData(); });
        download_ = Button(toolbar, "Download", [this] { Download(); });
        open_ = Button(toolbar, "Open download folder", [this] {
            if (const auto* app = Selected(); app && !app->downloaded.empty())
                wxLaunchDefaultApplication(W(path_utf8(app->downloaded.parent_path())));
        });
        cancel_ = Button(toolbar, "Cancel download", [this] { *cancelled_ = true; cancel_->Disable(); });
        cancel_->Hide();
        layout_->Add(toolbar, 0, wxEXPAND | wxBOTTOM, FromDIP(10));
        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        for (const auto& [label, width] : std::vector<std::pair<const char*,int>>{
            {"App", 175}, {"Repository", 145}, {"Version", 85}, {"Status", 150}, {"Package", 165}})
            list_->AppendColumn(label, wxLIST_FORMAT_LEFT, FromDIP(width));
        list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { Controls(); });
        list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { Controls(); });
        layout_->Add(list_, 1, wxEXPAND);
        progress_ = new wxGauge(this, wxID_ANY, 1000);
        layout_->Add(progress_, 0, wxEXPAND | wxTOP, FromDIP(8));
        progress_->Hide();
        FinishLayout();
        Controls();
    }

    ~AppsPage() override { *cancelled_ = true; }

private:
    const AvailableApp* Selected() const {
        const auto row = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        return row >= 0 && size_t(row) < apps_.size() ? &apps_[size_t(row)] : nullptr;
    }

    void Reset() override {
        *cancelled_ = true;
        apps_.clear();
        list_->DeleteAllItems();
    }

    void RefreshData() override {
        const auto directory = store_.directory();
        const auto result = std::make_shared<AppCatalog>();
        Work(wxS("Checking app repositories…"), [directory, result] {
            Store store(directory);
            *result = AppDownloads(store).catalog();
        }, [this, result] {
            const auto* selected = Selected();
            const auto selected_id = selected ? selected->repository.id + "/" + selected->app.id : "";
            apps_ = std::move(result->apps);
            list_->DeleteAllItems();
            for (size_t i = 0; i < apps_.size(); ++i) {
                const auto& app = apps_[i];
                const auto row = list_->InsertItem(long(i), W(app.app.name));
                list_->SetItem(row, 1, W(app.repository.name));
                list_->SetItem(row, 2, W(app.app.version));
                const auto state = !app.build ? "No build for this OS"
                    : !app.check_error.empty() ? "Check failed" : app.update ? "Update available"
                    : app.downloaded.empty() ? "Available" : "Downloaded";
                list_->SetItem(row, 3, state);
                if (app.build) list_->SetItem(row, 4, W(app.build->filename));
                if (selected_id == app.repository.id + "/" + app.app.id) list_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
            }
            if (!result->errors.empty()) status_->SetLabel(W(result->errors.front()));
            else if (apps_.empty()) status_->SetLabel("No apps found. Ask the server owner to share an app repository.");
        });
    }

    void Controls() override {
        if (!list_) return;
        const auto* app = Selected();
        refresh_->Enable(!busy_);
        download_->Enable(!busy_ && app && app->build);
        download_->SetLabel(app && app->update ? "Update" : app && !app->downloaded.empty() ? "Download again" : "Download");
        open_->Enable(!busy_ && app && !app->downloaded.empty());
        list_->Enable(!busy_);
        if (app && !app->check_error.empty() && !busy_ && status_->GetLabel().empty()) status_->SetLabel(W(app->check_error));
        if (!busy_) {
            progress_->Hide();
            cancel_->Hide();
        }
    }

    void Download() {
        const auto* app = Selected();
        if (!app || !app->build || busy_) return;
        const auto directory = store_.directory();
        const auto cancel = cancelled_ = std::make_shared<std::atomic<bool>>(false);
        const auto alive = alive_;
        auto* application = wxTheApp;
        progress_->SetValue(0);
        progress_->Show();
        cancel_->Enable();
        cancel_->Show();
        Work(wxS("Downloading and verifying package…"), [this, directory, app = *app, cancel, alive, application] {
            Store store(directory);
            auto last = std::chrono::steady_clock::time_point{};
            AppDownloads(store).download(app, [&](auto received, auto total) {
                if (!*alive || *cancel) return false;
                const auto now = std::chrono::steady_clock::now();
                if (now - last >= std::chrono::milliseconds(125)) {
                    last = now;
                    application->CallAfter([this, alive, received, total] {
                        if (!*alive) return;
                        if (total > 0) progress_->SetValue(int(1000.0 * double(received) / double(total)));
                        else progress_->Pulse();
                    });
                }
                return true;
            }, [alive, cancel] { return *alive && !*cancel; });
        }, [this] {
            progress_->Hide();
            cancel_->Hide();
            RefreshData();
        });
        Layout();
    }

    wxListCtrl* list_ = nullptr;
    wxButton *refresh_, *download_, *open_, *cancel_;
    wxGauge* progress_;
    std::vector<AvailableApp> apps_;
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);
};

class GitPage : public AsyncPage {
public:
    GitPage(wxWindow* parent, Store& store) : AsyncPage(parent, store, "Git") {
        auto* top = new wxBoxSizer(wxHORIZONTAL);
        refresh_ = Button(top, "Refresh", [this] { RefreshData(); });
        create_ = Button(top, wxS("New repository…"), [this] { Create(false); });
        publish_ = Button(top, wxS("Publish local repository…"), [this] { Create(true); });
        layout_->Add(top, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

        auto* columns = new wxBoxSizer(wxHORIZONTAL);
        list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(185, 285)), 0, nullptr, wxLB_SINGLE);
        list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { ShowRepository(); });
        columns->Add(list_, 0, wxEXPAND | wxRIGHT, FromDIP(12));
        auto* details = new wxBoxSizer(wxVERTICAL);
        heading_ = new wxStaticText(this, wxID_ANY, "Select a repository");
        heading_->SetFont(heading_->GetFont().Scaled(1.15f).Bold());
        details->Add(heading_, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        folder_label_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        details->Add(folder_label_, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        auto* local = new wxBoxSizer(wxHORIZONTAL);
        clone_ = Button(local, wxS("Clone…"), [this] { Clone(); });
        link_ = Button(local, wxS("Link local folder…"), [this] { Link(); });
        open_ = Button(local, "Open folder", [this] {
            if (!folder_.empty()) wxLaunchDefaultApplication(W(path_utf8(folder_)));
        });
        details->Add(local, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        auto* transfers = new wxBoxSizer(wxHORIZONTAL);
        fetch_ = Button(transfers, "Fetch", [this] { Transfer("fetch"); });
        pull_ = Button(transfers, "Pull", [this] { Transfer("pull"); });
        push_ = Button(transfers, "Push", [this] { Transfer("push"); });
        commit_ = Button(transfers, wxS("Commit all changes…"), [this] { Commit(); });
        details->Add(transfers, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        changes_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        auto font = changes_->GetFont();
        font.SetFamily(wxFONTFAMILY_TELETYPE);
        changes_->SetFont(font);
        details->Add(changes_, 1, wxEXPAND);
        columns->Add(details, 1, wxEXPAND);
        layout_->Add(columns, 1, wxEXPAND);
        FinishLayout();
        Controls();
    }

private:
    const GitRepository* Selected() const {
        const auto row = list_->GetSelection();
        return row >= 0 && size_t(row) < repositories_.size() ? &repositories_[size_t(row)] : nullptr;
    }

    void Reset() override {
        repositories_.clear();
        list_->Clear();
        folder_.clear();
        changes_->Clear();
        heading_->SetLabel("Select a repository");
        folder_label_->Clear();
    }

    void RefreshData() override {
        RefreshRepositories(Selected() ? Selected()->id : "");
    }

    void RefreshRepositories(std::string preferred) {
        const auto directory = store_.directory();
        const auto result = std::make_shared<std::vector<GitRepository>>();
        Work(wxS("Loading repositories…"), [directory, result] {
            Store store(directory);
            *result = git_repositories(store);
        }, [this, result, preferred = std::move(preferred)] {
            repositories_ = std::move(*result);
            list_->Clear();
            int index = repositories_.empty() ? wxNOT_FOUND : 0;
            for (size_t i = 0; i < repositories_.size(); ++i) {
                list_->Append(W(repositories_[i].name));
                if (repositories_[i].id == preferred) index = int(i);
            }
            list_->SetSelection(index);
            if (index == wxNOT_FOUND) status_->SetLabel("You have no repositories yet.");
            ShowRepository();
        });
    }

    void Controls() override {
        const auto* repo = Selected();
        const bool working_tree = !busy_ && repo && !folder_.empty() && fs::is_directory(folder_);
        list_->Enable(!busy_);
        refresh_->Enable(!busy_);
        create_->Enable(!busy_);
        publish_->Enable(!busy_);
        clone_->Enable(!busy_ && repo);
        link_->Enable(!busy_ && repo);
        open_->Enable(working_tree);
        fetch_->Enable(working_tree);
        pull_->Enable(working_tree);
        push_->Enable(working_tree && repo->role != "read");
        commit_->Enable(working_tree);
    }

    void ShowRepository() {
        folder_.clear();
        changes_->Clear();
        const auto* repo = Selected();
        heading_->SetLabel(repo ? W(repo->name + " · " + repo->role) : wxString("Select a repository"));
        if (repo) folder_ = git_folder(store_, repo->id);
        folder_label_->SetValue(folder_.empty() ? wxString("Clone or link a local Git folder") : W(path_utf8(folder_)));
        Controls();
        if (folder_.empty() || !fs::is_directory(folder_)) return;
        const auto output = std::make_shared<std::string>();
        Work(wxS("Reading working tree…"), [folder = folder_, output] { *output = git_status(folder); }, [this, output] {
            changes_->SetValue(W(*output));
        });
    }

    void Run(std::vector<std::string> args, fs::path cwd, std::function<void(Store&, const std::string&)> after,
             std::function<void()> done) {
        const auto directory = store_.directory();
        const auto result = std::make_shared<std::string>();
        Work(wxS("Running Git…"), [directory, result, args = std::move(args), cwd = std::move(cwd), after = std::move(after)] {
            Store store(directory);
            Desktop client(store);
            std::ostringstream log;
            const auto code = git_command(client, args, cwd, log, {}, {}, &log);
            *result = log.str();
            if (code) throw std::runtime_error(result->empty() ? "Git could not complete this operation." : *result);
            if (after) after(store, *result);
        }, [this, result, done = std::move(done)] {
            if (done) done();
            else if (!result->empty()) changes_->SetValue(W(*result));
        });
    }

    void Create(bool publish) {
        wxTextEntryDialog name(this, "Repository name", "Create private repository");
        if (name.ShowModal() != wxID_OK || name.GetValue().Trim().empty()) return;
        const auto text = U(name.GetValue().Trim());
        fs::path directory = fs::current_path();
        std::vector<std::string> args{"create", text};
        if (publish) {
            wxDirDialog choose(this, "Choose a local Git worktree to publish");
            if (choose.ShowModal() != wxID_OK) return;
            directory = path_from_utf8(U(choose.GetPath()));
            args.emplace_back("--push");
        }
        const auto id = std::make_shared<std::string>();
        Run(std::move(args), directory, [publish, directory, id](Store& store, const std::string& log) {
            // The create result is the first JSON value. A push may append Git's progress.
            std::istringstream input(log);
            nlohmann::json result;
            input >> result;
            *id = result.at("id");
            if (publish) link_git_folder(store, *id, directory);
        }, [this, id] { RefreshRepositories(*id); });
    }

    void Clone() {
        const auto* repo = Selected();
        if (!repo) return;
        wxDirDialog choose(this, "Choose the parent directory for the clone");
        if (choose.ShowModal() != wxID_OK) return;
        const auto parent = path_from_utf8(U(choose.GetPath()));
        if (repo->name.empty() || repo->name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
            throw std::runtime_error("The repository name cannot be used as a local folder.");
        const auto folder = parent / path_from_utf8(repo->name);
        Run({"clone", repo->id, path_utf8(folder)}, parent, [id = repo->id, folder](Store& store, const std::string&) {
            link_git_folder(store, id, folder);
        }, [this] { ShowRepository(); });
    }

    void Link() {
        const auto* repo = Selected();
        if (!repo) return;
        wxDirDialog choose(this, "Choose an existing local Git worktree");
        if (choose.ShowModal() != wxID_OK) return;
        const auto path = path_from_utf8(U(choose.GetPath()));
        const auto directory = store_.directory();
        Work(wxS("Checking working folder…"), [directory, id = repo->id, path] {
            Store store(directory);
            link_git_folder(store, id, path);
        }, [this] { ShowRepository(); });
    }

    void Transfer(const std::string& action) {
        if (const auto* repo = Selected(); repo && !folder_.empty())
            Run({action, repo->id}, folder_, {}, [this] { ShowRepository(); });
    }

    void Commit() {
        wxTextEntryDialog message(this, "Commit summary for all changes in this worktree", "Commit all changes");
        if (message.ShowModal() != wxID_OK || message.GetValue().Trim().empty()) return;
        Work(wxS("Committing changes…"), [folder = folder_, summary = U(message.GetValue().Trim())] {
            commit_git_changes(folder, summary);
        }, [this] { ShowRepository(); });
    }

    wxListBox* list_;
    wxStaticText* heading_;
    wxTextCtrl *folder_label_, *changes_;
    wxButton *refresh_, *create_, *publish_, *clone_, *link_, *open_, *fetch_, *pull_, *push_, *commit_;
    std::vector<GitRepository> repositories_;
    fs::path folder_;
};

}
wxPanel* build_apps_page(wxWindow* parent, Store& store) { return new AppsPage(parent, store); }
void activate_apps_page(wxPanel* page) { static_cast<AppsPage*>(page)->Activate(); }
wxPanel* build_git_page(wxWindow* parent, Store& store) { return new GitPage(parent, store); }
void activate_git_page(wxPanel* page) { static_cast<GitPage*>(page)->Activate(); }
}
