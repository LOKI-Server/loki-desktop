#include "workspace_pages.hpp"

#include <wx/wx.h>
#include <wx/choicdlg.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/statline.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/wrapsizer.h>

#include "commands.hpp"
#include "ui.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace loki {
namespace {

wxString W(const std::string& text) { return wxString::FromUTF8(text); }
std::string U(const wxString& text) { return text.utf8_string(); }

std::string diff_text(const std::vector<RepositoryDiff>& files, const std::string& no_patch = "Text diff unavailable.") {
    if (files.empty()) return "No file changes.";
    std::ostringstream text;
    for (const auto& file : files) {
        text << file.change << "  " << file.path << '\n';
        if (file.patch) text << *file.patch;
        else if (!no_patch.empty()) text << no_patch << '\n';
        text << '\n';
    }
    return text.str();
}

wxString commit_label(const RepositoryCommit& commit) {
    return W(commit.id.substr(0, 9) + "  " + commit.message.substr(0, commit.message.find('\n')) + "  [" + commit.author + "]");
}

bool inspect_repository_text(wxWindow* parent, const wxString& title,
                             const std::vector<std::pair<wxString, std::string>>& pages,
                             const wxString& action = {}) {
    wxDialog dialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* tabs = pages.size() > 1 ? new wxNotebook(&dialog, wxID_ANY) : nullptr;
    for (const auto& [label, text] : pages) {
        auto* content = new wxTextCtrl(tabs ? static_cast<wxWindow*>(tabs) : &dialog, wxID_ANY, W(text),
                                       wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        auto font = content->GetFont();
        font.SetFamily(wxFONTFAMILY_TELETYPE);
        content->SetFont(font);
        if (tabs) tabs->AddPage(content, label);
        else layout->Add(content, 1, wxEXPAND | wxALL, dialog.FromDIP(10));
    }
    if (tabs) layout->Add(tabs, 1, wxEXPAND | wxALL, dialog.FromDIP(10));
    auto* buttons = dialog.CreateStdDialogButtonSizer(action.empty() ? wxOK : wxOK | wxCANCEL);
    auto* ok = static_cast<wxButton*>(dialog.FindWindow(wxID_OK));
    ok->SetLabel(action.empty() ? wxString("Close") : action);
    if (!action.empty()) static_cast<wxButton*>(dialog.FindWindow(wxID_CANCEL))->SetDefault();
    layout->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(10));
    dialog.SetSizer(layout);
    dialog.SetMinSize(dialog.FromDIP(wxSize(460, 280)));
    dialog.SetSize(dialog.FromDIP(wxSize(760, 520)));
    dialog.CentreOnParent();
    return dialog.ShowModal() == wxID_OK && !action.empty();
}

class AsyncPage : public wxPanel {
public:
    AsyncPage(wxWindow* parent, Store& store, const wxString& title, const wxString& subtitle)
        : wxPanel(parent), store_(store) {
        layout_ = new wxBoxSizer(wxVERTICAL);
        auto header = ui::page_header(this, title, subtitle);
        actions_ = header.actions;
        layout_->Add(header.sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(16));
    }
    ~AsyncPage() override { *alive_ = false; }
    bool Busy() const { return busy_; }

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
        status_ = new ui::Status(this);
        layout_->Add(status_, 0, wxEXPAND | wxTOP, FromDIP(10));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(layout_, 1, wxEXPAND | wxALL, FromDIP(24));
        SetSizer(outer);
    }

    wxButton* Button(wxSizer* row, const wxString& title, std::function<void()> action) {
        return ui::button(this, row, title, std::move(action));
    }

    void Work(const wxString& working, std::function<void()> job, std::function<void()> done) {
        if (busy_) return;
        busy_ = true;
        status_->Info(working);
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
                    status_->Clear();
                    Controls();
                    Layout();
                    if (refresh_queued_) { refresh_queued_ = false; RefreshData(); }
                    return;
                }
                if (error.empty()) {
                    status_->Clear();
                    try { done(); }
                    catch (const std::exception& e) { status_->Error(W(e.what())); }
                } else {
                    status_->Error(W(error));
                }
                Controls();
                Layout();
            });
        }).detach();
    }

    Store& store_;
    wxBoxSizer* layout_;
    wxBoxSizer* actions_;
    ui::Status* status_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    bool busy_ = false;
private:
    std::string session_;
    bool refresh_queued_ = false;
};

class RepositoryPage : public AsyncPage {
public:
    RepositoryPage(wxWindow* parent, Store& store)
        : AsyncPage(parent, store, "Repositories", "Private Skald repositories on your LOKI server") {
        refresh_ = Button(actions_, "Refresh", [this] { RefreshData(); });
        publish_ = Button(actions_, wxS("Publish local…"), [this] { Create(true); });
        create_ = Button(actions_, wxS("New repository…"), [this] { Create(false); });

        auto* columns = new wxBoxSizer(wxHORIZONTAL);
        list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(230, -1)), 0, nullptr, wxLB_SINGLE);
        list_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { ShowRepository(); });
        columns->Add(list_, 0, wxEXPAND | wxRIGHT, FromDIP(20));

        auto* details = new wxBoxSizer(wxVERTICAL);
        heading_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                    wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        heading_->SetFont(heading_->GetFont().Scaled(1.3f).Bold());
        details->Add(heading_, 0, wxEXPAND);
        role_ = new wxStaticText(this, wxID_ANY, "");
        role_->SetForegroundColour(ui::muted());
        details->Add(role_, 0, wxEXPAND | wxTOP, FromDIP(2));
        location_ = new wxBoxSizer(wxHORIZONTAL);
        folder_label_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                        wxST_ELLIPSIZE_MIDDLE | wxST_NO_AUTORESIZE);
        folder_label_->SetForegroundColour(ui::muted());
        location_->Add(folder_label_, 1, wxALIGN_CENTER_VERTICAL);
        open_ = Button(location_, "Open folder", [this] {
            if (!folder_.empty()) wxLaunchDefaultApplication(W(path_utf8(folder_)));
        });
        details->Add(location_, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));

        // Before the repository is on this computer: get it here.
        local_ = new wxBoxSizer(wxHORIZONTAL);
        clone_ = Button(local_, wxS("Clone…"), [this] { Clone(); });
        link_ = Button(local_, wxS("Link existing folder…"), [this] { Link(); });
        details->Add(local_, 0, wxBOTTOM, FromDIP(12));

        linked_ = new wxBoxSizer(wxVERTICAL);
        AddSection("Sync");
        sync_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        sync_->SetForegroundColour(ui::muted());
        linked_->Add(sync_, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        auto* transfers = new wxBoxSizer(wxHORIZONTAL);
        fetch_ = Button(transfers, "Fetch", [this] { Transfer(RepositoryTransfer::Fetch); });
        pull_ = Button(transfers, "Pull", [this] { Transfer(RepositoryTransfer::Pull); });
        push_ = Button(transfers, "Push", [this] { Transfer(RepositoryTransfer::Push); });
        linked_->Add(transfers, 0, wxEXPAND | wxBOTTOM, FromDIP(16));

        AddSection("Branches");
        auto* branches = new wxBoxSizer(wxHORIZONTAL);
        branch_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(105, -1)));
        branch_->SetName("Local branches");
        branch_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { Controls(); });
        branches->Add(branch_, 1, wxALIGN_CENTER_VERTICAL);
        switch_branch_ = Button(branches, "Switch", [this] { SwitchBranch(); });
        branch_actions_ = Button(branches, wxS("Branch actions ▾"), [this] { ShowBranchActions(); });
        linked_->Add(branches, 0, wxEXPAND | wxBOTTOM, FromDIP(16));

        AddSection("Working changes");
        auto* staging = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
        diff_ = Button(staging, wxS("Diff…"), [this] { WorkingDiff(); });
        stage_ = Button(staging, wxS("Stage…"), [this] { StageFiles(true); });
        commit_ = Button(staging, wxS("Commit staged…"), [this] { Commit(false); });
        change_actions_ = Button(staging, wxS("More ▾"), [this] { ShowChangeActions(); });
        change_actions_->SetName("More working changes actions");
        linked_->Add(staging, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

        clean_ = new wxStaticText(this, wxID_ANY, "Working copy is clean.");
        clean_->SetForegroundColour(ui::muted());
        linked_->Add(clean_, 0, wxBOTTOM, FromDIP(4));
        changes_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                  wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        auto font = changes_->GetFont();
        font.SetFamily(wxFONTFAMILY_TELETYPE);
        changes_->SetFont(font);
        linked_->Add(changes_, 1, wxEXPAND);
        details->Add(linked_, 1, wxEXPAND);
        columns->Add(details, 1, wxEXPAND);
        layout_->Add(columns, 1, wxEXPAND);
        progress_ = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, FromDIP(wxSize(-1, 6)));
        layout_->Add(progress_, 0, wxEXPAND | wxTOP, FromDIP(10));
        progress_->Hide();
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { progress_->Pulse(); }, progress_timer_.GetId());
        FinishLayout();
        ShowDetails();
        Controls();
    }

private:
    void AddSection(const wxString& title) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* label = new wxStaticText(this, wxID_ANY, title);
        label->SetFont(label->GetFont().Bold());
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        row->Add(new wxStaticLine(this), 1, wxALIGN_CENTER_VERTICAL);
        linked_->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    }

    const HostedRepository* Selected() const {
        const auto row = list_->GetSelection();
        return row >= 0 && size_t(row) < repositories_.size() ? &repositories_[size_t(row)] : nullptr;
    }

    bool OtherBranchSelected() const {
        const auto index = branch_->GetSelection();
        return working_status_ && index != wxNOT_FOUND &&
               working_status_->current_branch != working_status_->branches[size_t(index)].name;
    }

    wxString SyncText() const {
        if (!working_status_) return "";
        if (!working_status_->current_branch) {
            if (working_status_->revision) return W("Detached at " + working_status_->revision->substr(0, 9));
            return "Detached working copy";
        }
        switch (working_status_->sync) {
        case BranchSync::LocalOnly:
            return working_status_->revision ? "Local-only branch" : "Local-only branch. Make a commit to publish.";
        case BranchSync::UpToDate: return "Up to date with the remote branch";
        case BranchSync::Ahead: return "Ahead of the remote branch";
        case BranchSync::Behind: return "Behind the remote branch";
        case BranchSync::Diverged: return "Diverged from the remote branch";
        case BranchSync::Unknown:
            return working_status_->remote_branches.empty() ? "Fetch to check remote branch status"
                                                            : "Remote branch status unavailable";
        }
        return "";
    }

    void ShowChanges(bool linked) {
        const bool loaded = linked && working_status_;
        const bool has_changes = loaded && (working_status_->merging || !working_status_->staged.empty() ||
                                 !working_status_->unstaged.empty() || !working_status_->conflicts.empty());
        clean_->Show(loaded && !has_changes);
        changes_->Show(has_changes);
    }

    void MenuAction(wxMenu& menu, const wxString& label, std::function<void()> action, bool enabled = true) {
        auto* item = menu.Append(wxID_ANY, label);
        item->Enable(enabled);
        menu.Bind(wxEVT_MENU, [action = std::move(action)](wxCommandEvent&) { action(); }, item->GetId());
    }

    void ShowBranchActions() {
        if (!working_status_) return;
        wxMenu menu;
        MenuAction(menu, wxS("New branch…"), [this] { NewBranch(); });
        MenuAction(menu, wxS("Remote branches…"), [this] { RemoteBranches(); });
        MenuAction(menu, wxS("Compare branches…"), [this] { CompareBranches(); });
        MenuAction(menu, wxS("History…"), [this] { History(); }, working_status_->revision.has_value());
        menu.AppendSeparator();
        MenuAction(menu, wxS("Delete selected local branch…"), [this] { DeleteBranch(); }, OtherBranchSelected());
        branch_actions_->PopupMenu(&menu, wxPoint(0, branch_actions_->GetSize().y));
    }

    bool CanCommitAll() const {
        return working_status_ && working_status_->current_branch && !working_status_->merging &&
            (!working_status_->staged.empty() || !working_status_->unstaged.empty());
    }

    void ShowChangeActions() {
        if (!working_status_) return;
        const auto& state = *working_status_;
        wxMenu menu;
        MenuAction(menu, wxS("Unstage files…"), [this] { StageFiles(false); }, !state.merging && !state.staged.empty());
        MenuAction(menu, wxS("Commit all changes…"), [this] { Commit(true); }, CanCommitAll());
        if (state.merging) {
            menu.AppendSeparator();
            MenuAction(menu, wxS("Abort merge…"), [this] { AbortMerge(); });
        }
        change_actions_->PopupMenu(&menu, wxPoint(0, change_actions_->GetSize().y));
    }

    void Reset() override {
        repositories_.clear();
        list_->Clear();
        folder_.clear();
        working_status_.reset();
        branch_->Clear();
        changes_->Clear();
        sync_->SetLabel("");
        ShowDetails();
    }

    void RefreshData() override {
        RefreshRepositories(Selected() ? Selected()->id : "");
    }

    void RefreshRepositories(std::string preferred) {
        const auto directory = store_.directory();
        const auto result = std::make_shared<HostedRepositories>();
        Work(wxS("Loading repositories…"), [directory, result] {
            Store store(directory);
            *result = hosted_repositories(store);
        }, [this, result, preferred = std::move(preferred)] {
            repositories_ = std::move(result->repositories);
            owners_ = std::move(result->owners);
            list_->Clear();
            int index = repositories_.empty() ? wxNOT_FOUND : 0;
            for (size_t i = 0; i < repositories_.size(); ++i) {
                list_->Append(W(repositories_[i].label()));
                if (repositories_[i].id == preferred) index = int(i);
            }
            list_->SetSelection(index);
            if (result->transfers_waiting)
                status_->Info(W(result->transfers_waiting == 1
                    ? std::string("A repository transfer is waiting for you. Accept or decline it on the "
                                  "Repositories page of LOKI in your browser.")
                    : std::to_string(result->transfers_waiting) + " repository transfers are waiting for you. Accept "
                      "or decline them on the Repositories page of LOKI in your browser."));
            ShowRepository();
        });
    }

    void Controls() override {
        const auto* repo = Selected();
        const bool working_tree = !busy_ && repo && !folder_.empty() && fs::is_directory(folder_);
        list_->Enable(!busy_);
        refresh_->Enable(!busy_);
        create_->Enable(!busy_ && !owners_.empty());
        publish_->Enable(!busy_ && !owners_.empty());
        clone_->Enable(!busy_ && repo);
        link_->Enable(!busy_ && repo);
        open_->Enable(working_tree);
        fetch_->Enable(working_tree);
        const bool ready = working_tree && working_status_;
        const bool merging = working_status_ && working_status_->merging;
        const bool on_branch = ready && working_status_->current_branch.has_value();
        pull_->Enable(ready && !merging);
        push_->Enable(on_branch && working_status_->revision && repo->role != "read");
        const wxString push_text = working_status_ && working_status_->sync == BranchSync::LocalOnly
            ? "Publish branch" : "Push";
        if (push_->GetLabel() != push_text) push_->SetLabel(push_text);
        stage_->Enable(ready && (!working_status_->unstaged.empty() || !working_status_->conflicts.empty()));
        commit_->Enable(on_branch && working_status_->conflicts.empty() &&
                        (merging || !working_status_->staged.empty()));
        change_actions_->Enable(ready && (merging || !working_status_->staged.empty() || CanCommitAll()));
        diff_->Enable(ready);
        branch_actions_->Enable(ready);
        const bool can_choose = ready && !working_status_->branches.empty();
        branch_->Enable(can_choose);
        switch_branch_->Enable(can_choose && !merging && OtherBranchSelected());
        if (progress_->IsShown() != busy_) {
            progress_->Show(busy_);
            if (busy_) {
                progress_->Pulse();
                progress_timer_.Start(100);
            } else {
                progress_timer_.Stop();
            }
            Layout();
        }
    }

    void ShowRepository(const std::string& preferred_branch = {}, const std::string& error = {}) {
        folder_.clear();
        working_status_.reset();
        branch_->Clear();
        changes_->Clear();
        sync_->SetLabel("");
        const auto* repo = Selected();
        if (repo) folder_ = repository_folder(store_, repo->id);
        ShowDetails();
        Controls();
        if (folder_.empty() || !fs::is_directory(folder_)) return;
        const auto output = std::make_shared<RepositoryStatus>();
        Work(wxS("Reading working tree…"), [directory = store_.directory(), id = repo->id, folder = folder_, output] {
            Store store(directory);
            *output = hosted_repository_status(store, id, folder);
        }, [this, output, preferred_branch, error] {
            working_status_ = std::move(*output);
            const auto& state = *working_status_;
            int selected = wxNOT_FOUND;
            for (const auto& branch : state.branches) {
                const auto& name = branch.name;
                const bool current = state.current_branch == name;
                const auto index = branch_->Append(W(current ? name + " (current)" : name));
                if (preferred_branch.empty() ? current : name == preferred_branch) selected = index;
            }
            changes_->SetValue(W(state.changes));
            ShowChanges(true);
            branch_->SetSelection(selected);
            sync_->SetLabel(SyncText());
            sync_->SetToolTip(state.current_branch ? wxString("Based on the most recent sync with this remote.")
                                                   : W(state.revision.value_or("")));
            if (!error.empty()) status_->Error(W(error));
            else if (!state.conflicts.empty()) status_->Info("Resolve the listed conflicts, then stage each resolved file.");
        });
    }

    /** Fills the right-hand side for the selected repository, or explains what to do without one. */
    void ShowDetails() {
        const auto* repo = Selected();
        const bool linked = repo && !folder_.empty() && fs::is_directory(folder_);
        if (!repo) {
            heading_->SetLabel(repositories_.empty() ? "No repositories yet" : "Select a repository");
            role_->SetLabel(repositories_.empty()
                ? wxS("Create one with New repository, or publish a Skald folder you already have.")
                : wxS(""));
        } else {
            heading_->SetLabel(W(repo->label()));
            role_->SetLabel(repo->role == "manage" ? "You manage this repository"
                            : repo->role == "write" ? "You can push changes" : "Read only");
        }
        heading_->SetToolTip(heading_->GetLabel());
        folder_label_->SetLabel(!repo ? wxString()
            : linked ? W(path_utf8(folder_))
            : wxString("Not on this computer yet. Clone it, or link a Skald folder that already has it."));
        folder_label_->SetToolTip(folder_label_->GetLabel());
        location_->ShowItems(repo != nullptr);
        open_->Show(linked);
        local_->ShowItems(repo && !linked);
        linked_->ShowItems(linked);
        ShowChanges(linked);
        Layout();
    }

    void Create(bool publish) {
        if (owners_.empty()) return;
        // Ask who owns it only when a group is possible; otherwise it is yours.
        std::string group = owners_.front().group;
        if (owners_.size() > 1) {
            wxArrayString labels;
            for (const auto& owner : owners_) labels.Add(W(owner.label));
            wxSingleChoiceDialog owner(this, "Who should own the new repository?",
                                       publish ? "Publish local repository" : "New repository", labels);
            if (owner.ShowModal() != wxID_OK) return;
            group = owners_[size_t(owner.GetSelection())].group;
        }
        wxTextEntryDialog name(this, "Repository name (lowercase letters, digits, and dashes)",
                               publish ? "Publish local repository" : "New repository");
        if (name.ShowModal() != wxID_OK || name.GetValue().Trim().empty()) return;
        const auto text = U(name.GetValue().Trim());
        std::optional<fs::path> folder;
        if (publish) {
            wxDirDialog choose(this, "Choose a local Skald working copy to publish");
            if (choose.ShowModal() != wxID_OK) return;
            folder = path_from_utf8(U(choose.GetPath()));
        }
        const auto id = std::make_shared<std::string>();
        Work(publish ? wxS("Publishing repository…") : wxS("Creating repository…"),
             [directory = store_.directory(), text, folder, group, id] {
            Store store(directory);
            *id = create_hosted_repository(store, text, folder, group);
        }, [this, id] { RefreshRepositories(*id); });
    }

    void Clone() {
        const auto* repo = Selected();
        if (!repo) return;
        wxDirDialog choose(this, "Choose the parent directory for the clone");
        if (choose.ShowModal() != wxID_OK) return;
        const auto parent = path_from_utf8(U(choose.GetPath()));
        if (repo->name.empty() || repo->name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos) {
            status_->Error("The repository name cannot be used as a local folder.");
            return;
        }
        const auto folder = parent / path_from_utf8(repo->name);
        Work(wxS("Cloning repository…"), [directory = store_.directory(), id = repo->id, folder] {
            Store store(directory);
            clone_hosted_repository(store, id, folder);
        }, [this] { ShowRepository(); });
    }

    void Link() {
        const auto* repo = Selected();
        if (!repo) return;
        const auto id = repo->id;
        wxDirDialog choose(this, "Choose a local folder to link", "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (choose.ShowModal() != wxID_OK) return;
        const auto path = path_from_utf8(U(choose.GetPath()));
        const auto result = std::make_shared<RepositoryFolder>();
        Work(wxS("Checking working folder…"), [path, result] {
            *result = inspect_repository_folder(path);
        }, [this, id, result] { LinkFolder(id, *result); });
    }

    void LinkFolder(const std::string& id, const RepositoryFolder& folder) {
        if (!folder.initialized) {
            wxMessageDialog prompt(this,
                W("This folder is not a Skald working copy. Would you like to create one here?\n\n"
                  + path_utf8(folder.root)),
                "Create a Skald working copy", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
            prompt.SetYesNoLabels("Create and link", "Cancel");
            if (prompt.ShowModal() != wxID_YES) return;
        }
        const auto directory = store_.directory();
        Work(folder.initialized ? wxS("Linking folder…") : wxS("Creating Skald working copy…"), [directory, id, folder] {
            Store store(directory);
            if (!folder.initialized) initialize_repository_folder(folder.root);
            link_repository_folder(store, id, folder.root);
        }, [this, folder] { OfferIgnoreRename(folder); });
    }

    void OfferIgnoreRename(const RepositoryFolder& folder) {
        if (folder.can_rename_ignore) {
            wxMessageDialog prompt(this,
                "This folder has a .gitignore. Rename it to .skaldignore so Skald can use it? "
                "Git uses the original filename.",
                "Use these ignore rules in Skald", wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
            prompt.SetYesNoLabels("Rename", "Keep .gitignore");
            if (prompt.ShowModal() == wxID_YES) {
                UpdateWorkingCopy(wxS("Renaming ignore file…"), [root = folder.root] { rename_repository_ignore(root); });
                return;
            }
        }
        ShowRepository();
    }

    void Transfer(RepositoryTransfer transfer) {
        const auto* repo = Selected();
        if (!repo || folder_.empty()) return;
        const auto working = transfer == RepositoryTransfer::Fetch ? wxS("Fetching repository data…")
            : transfer == RepositoryTransfer::Pull ? wxS("Pulling repository changes…") : wxS("Pushing repository changes…");
        UpdateWorkingCopy(working, [directory = store_.directory(), id = repo->id, folder = folder_, transfer] {
            Store store(directory);
            transfer_repository(store, id, folder, transfer);
        });
    }

    void UpdateWorkingCopy(const wxString& working, std::function<void()> job, const std::string& preferred = {}) {
        const auto error = std::make_shared<std::string>();
        Work(working, [job = std::move(job), error] {
            try { job(); }
            catch (const std::exception& exception) { *error = exception.what(); }
        }, [this, error, preferred] { ShowRepository(preferred, *error); });
    }

    std::string Author() const {
        const auto session = store_.session();
        return session.display_name.empty() ? session.username : session.display_name;
    }

    void Commit(bool all) {
        wxTextEntryDialog message(this, "Commit summary", all ? "Commit all changes" : "Commit staged changes");
        if (message.ShowModal() != wxID_OK || message.GetValue().Trim().empty()) return;
        UpdateWorkingCopy(wxS("Committing changes…"),
                          [folder = folder_, summary = U(message.GetValue().Trim()), author = Author(), all] {
            if (all) commit_repository_changes(folder, summary, author);
            else commit_staged_repository_changes(folder, summary, author);
        });
    }

    void StageFiles(bool stage) {
        if (!working_status_) return;
        auto paths = stage ? working_status_->unstaged : working_status_->staged;
        if (stage) {
            paths.insert(paths.end(), working_status_->conflicts.begin(), working_status_->conflicts.end());
            std::ranges::sort(paths);
            paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        }
        wxArrayString labels;
        for (const auto& path : paths) labels.Add(W(path));
        wxMultiChoiceDialog dialog(this, stage ? "Select files to stage for the next commit" : "Select files to unstage",
                                   stage ? "Stage files" : "Unstage files", labels);
        dialog.FindWindow(wxID_OK)->SetLabel(stage ? "Stage" : "Unstage");
        if (dialog.ShowModal() != wxID_OK || dialog.GetSelections().empty()) return;
        std::vector<std::string> selected;
        for (const auto index : dialog.GetSelections()) selected.push_back(paths[size_t(index)]);
        UpdateWorkingCopy(stage ? wxS("Staging files…") : wxS("Unstaging files…"),
                          [folder = folder_, selected = std::move(selected), stage] {
            stage_repository_paths(folder, selected, stage);
        });
    }

    void WorkingDiff() {
        using Diffs = std::pair<std::vector<RepositoryDiff>, std::vector<RepositoryDiff>>;
        const auto result = std::make_shared<Diffs>();
        Work(wxS("Reading working changes…"), [folder = folder_, result] {
            result->first = repository_working_diff(folder, false);
            result->second = repository_working_diff(folder, true);
        }, [this, result] {
            inspect_repository_text(this, "Working copy diff",
                                    {{"Unstaged", diff_text(result->first)}, {"Staged", diff_text(result->second)}});
        });
    }

    void History() {
        const auto commits = std::make_shared<std::vector<RepositoryCommit>>();
        Work(wxS("Reading commit history…"), [folder = folder_, commits] {
            *commits = repository_history(folder);
        }, [this, commits] {
            if (commits->empty()) {
                status_->Info("This working copy has no commits yet.");
                return;
            }
            wxArrayString labels;
            for (const auto& commit : *commits) labels.Add(commit_label(commit));
            wxSingleChoiceDialog dialog(this, "Select a commit to see its changes", "Recent commits", labels);
            dialog.FindWindow(wxID_OK)->SetLabel("View changes");
            dialog.SetSelection(0);
            if (dialog.ShowModal() != wxID_OK) return;
            const auto commit = (*commits)[size_t(dialog.GetSelection())];
            const auto files = std::make_shared<std::vector<RepositoryDiff>>();
            Work(wxS("Reading commit changes…"), [folder = folder_, commit, files] {
                *files = repository_commit_diff(folder, commit);
            }, [this, commit, files] {
                const auto date = wxDateTime(static_cast<time_t>(commit.timestamp));
                const auto time = date.IsValid() ? U(date.FormatISOCombined(' ')) : std::to_string(commit.timestamp);
                const auto summary = commit.id + "\n" + commit.author + "  " + time + "\n\n" + commit.message + "\n\n";
                const auto details = commit.parents.size() > 1 ? "Changes relative to the first parent\n\n" : "";
                inspect_repository_text(this, "Commit " + W(commit.id.substr(0, 9)), {{"Changes",
                    summary + details + diff_text(*files, commit.parents.empty() ? "" : "Text diff unavailable.")}});
            });
        });
    }

    void RemoteBranches() {
        if (!working_status_) return;
        const auto& state = *working_status_;
        if (state.remote_branches.empty()) {
            status_->Info("Fetch to update the list of remote branches.");
            return;
        }
        wxArrayString labels;
        for (const auto& remote : state.remote_branches) {
            const auto local = std::ranges::find(state.branches, remote.name, &RepositoryBranch::name);
            labels.Add(W(remote.name + (local == state.branches.end() ? " (remote only)" : " (local branch exists)")));
        }
        wxSingleChoiceDialog dialog(this, "Select a remote branch to check out", "Remote branches", labels);
        dialog.FindWindow(wxID_OK)->SetLabel("Check out");
        dialog.SetSelection(0);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto remote = state.remote_branches[size_t(dialog.GetSelection())];
        const auto local = std::ranges::find(state.branches, remote.name, &RepositoryBranch::name);
        if (local != state.branches.end() && local->revision == remote.revision) {
            if (state.current_branch == remote.name) {
                status_->Info("This branch is already checked out.");
                return;
            }
            UpdateWorkingCopy(wxS("Switching branches…"), [folder = folder_, name = remote.name] {
                switch_repository_branch(folder, name);
            });
            return;
        }

        auto name = remote.name;
        if (local != state.branches.end()) {
            wxTextEntryDialog name_dialog(this, "That name is in use. Choose a local name for the fetched revision.",
                                          "Check out remote branch", W(name + "-remote"));
            if (name_dialog.ShowModal() != wxID_OK) return;
            name = U(name_dialog.GetValue().Trim().Trim(false));
            if (name.empty()) return;
        }
        UpdateWorkingCopy(wxS("Checking out remote branch…"), [folder = folder_, name, revision = remote.revision.value()] {
            create_repository_branch(folder, name, revision);
            try { switch_repository_branch(folder, name); }
            catch (const std::runtime_error& error) {
                throw std::runtime_error("Created local branch " + name + ". " + error.what());
            }
        }, name);
    }

    void CompareBranches() {
        if (!working_status_) return;
        const auto& state = *working_status_;
        wxArrayString labels;
        std::vector<RepositoryBranch> targets;
        for (const auto& branch : state.branches) {
            if (branch.revision && state.current_branch != branch.name) {
                labels.Add(W(branch.name + " (local)"));
                targets.push_back(branch);
            }
        }
        for (const auto& remote : state.remote_branches) {
            labels.Add(W(remote.name + " (remote)"));
            targets.push_back(remote);
        }
        if (targets.empty()) {
            status_->Info("Create another branch or fetch remote branches to compare.");
            return;
        }
        const auto branch = state.current_branch;
        const auto current = state.revision;
        wxSingleChoiceDialog choose(this, "Compare the current branch with", "Compare branches", labels);
        choose.FindWindow(wxID_OK)->SetLabel("Compare");
        choose.SetSelection(0);
        if (choose.ShowModal() != wxID_OK) return;
        const auto index = size_t(choose.GetSelection());
        const auto target = targets[index].revision.value();
        const auto target_name = targets[index].name;
        const auto label = labels[index];
        const auto result = std::make_shared<BranchComparison>();
        Work(wxS("Comparing branches…"), [folder = folder_, current, target, result] {
            *result = compare_repository_branch(folder, current, target);
        }, [this, result, branch, current, target, target_name, label] {
            std::string commits;
            if (result->history_limited) commits = "The commit history exceeds the comparison limit. File changes are available.";
            else {
                auto append = [&](const std::string& heading, const std::vector<RepositoryCommit>& entries) {
                    commits += heading + "\n";
                    if (entries.empty()) commits += "None\n";
                    for (const auto& entry : entries) commits += U(commit_label(entry)) + "\n";
                    commits += "\n";
                };
                append("Commits only on the current branch", result->current_only);
                append("Commits only on " + U(label), result->incoming);
            }
            const bool can_merge = branch && current != target && !working_status_->merging &&
                (result->history_limited || !result->incoming.empty());
            const auto title = "Compare " + W(branch.value_or("detached revision")) + " with " + label;
            if (inspect_repository_text(this, title, {{"Commits", commits}, {"Files", diff_text(result->files)}},
                                        can_merge ? "Merge into current branch" : "")) {
                UpdateWorkingCopy(wxS("Merging branches…"),
                                  [folder = folder_, branch, current, target, target_name, author = Author()] {
                    merge_repository_branch(folder, branch, current, target, author, "Merge " + target_name);
                });
            }
        });
    }

    void AbortMerge() {
        if (wxMessageBox("Abort this merge and undo its changes?", "Abort merge",
                         wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
        UpdateWorkingCopy(wxS("Aborting merge…"), [folder = folder_] { abort_repository_merge(folder); });
    }

    void NewBranch() {
        wxTextEntryDialog dialog(this, "Name for the local branch at the current revision", "New branch");
        if (dialog.ShowModal() != wxID_OK) return;
        const auto name = U(dialog.GetValue().Trim().Trim(false));
        if (name.empty()) return;
        Work(wxS("Creating local branch…"), [folder = folder_, name] {
            create_repository_branch(folder, name);
        }, [this, name] { ShowRepository(name); });
    }

    void SwitchBranch() {
        if (!OtherBranchSelected()) return;
        const auto name = working_status_->branches[size_t(branch_->GetSelection())].name;
        Work(wxS("Switching branches…"), [folder = folder_, name] {
            switch_repository_branch(folder, name);
        }, [this] { ShowRepository(); });
    }

    void DeleteBranch() {
        if (!OtherBranchSelected()) return;
        const auto name = working_status_->branches[size_t(branch_->GetSelection())].name;
        if (wxMessageBox(W("Delete the local branch “" + name + "”?"), "Delete local branch",
                         wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
        Work(wxS("Deleting local branch…"), [folder = folder_, name] {
            delete_repository_branch(folder, name);
        }, [this] { ShowRepository(); });
    }

    wxListBox* list_;
    wxStaticText *heading_, *role_, *folder_label_, *sync_, *clean_;
    wxTextCtrl* changes_;
    wxBoxSizer *local_, *location_, *linked_;
    wxButton *refresh_, *create_, *publish_, *clone_, *link_, *open_, *fetch_, *pull_, *push_;
    wxButton *branch_actions_, *change_actions_, *diff_, *stage_, *commit_;
    wxChoice* branch_;
    wxButton* switch_branch_;
    std::optional<RepositoryStatus> working_status_;
    wxGauge* progress_;
    wxTimer progress_timer_{this};
    std::vector<HostedRepository> repositories_;
    std::vector<RepositoryOwner> owners_;
    fs::path folder_;
};

}
wxPanel* build_repository_page(wxWindow* parent, Store& store) { return new RepositoryPage(parent, store); }
void activate_repository_page(wxPanel* page) { static_cast<RepositoryPage*>(page)->Activate(); }
bool workspace_page_busy(const wxPanel* page) { return static_cast<const AsyncPage*>(page)->Busy(); }
}
