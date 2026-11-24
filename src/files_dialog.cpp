#include "files_dialog.hpp"
#include "files_api.hpp"
#include "file_downloads.hpp"
#include "ui.hpp"
#include <wx/wx.h>
#include <wx/filedlg.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/utils.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace fs = std::filesystem;
namespace loki {
namespace {
wxString W(const std::string& value) { return wxString::FromUTF8(value); }
std::string U(const wxString& value) { return value.utf8_string(); }
wxString size_text(std::int64_t size) { return wxFileName::GetHumanReadableSize(wxULongLong(size),"0 B",1); }
/** Keeps list notifications from re-entering the dialog while rows are replaced. */
struct Hold {
    bool& flag;
    explicit Hold(bool& flag) : flag(flag) { flag = true; }
    ~Hold() { flag = false; }
};

class FilesPage : public wxPanel {
public:
    FilesPage(wxWindow* parent, Store& store)
        : wxPanel(parent), account_(store) {
        auto* contents = new wxBoxSizer(wxVERTICAL);
        auto header = ui::page_header(this, "Browse files", "Shared folders on your LOKI server");
        refresh_ = ui::button(this, header.actions, "Refresh", [this] {
            if (share_.empty()) LoadShares(); else LoadEntries(path_, page_);
        });
        contents->Add(header.sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(16));

        auto* nav = new wxBoxSizer(wxHORIZONTAL);
        up_ = ui::button(this, nav, wxS("↑ Up"), [this] {
            if (share_.empty()) return;
            if (path_.empty()) { LoadShares(); return; }
            const auto slash = path_.find_last_of('/');
            LoadEntries(slash == std::string::npos ? "" : path_.substr(0, slash), 0);
        });
        nav->AddSpacer(FromDIP(8));
        location_ = new wxTextCtrl(this, wxID_ANY, "Shares", wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        nav->Add(location_, 1, wxALIGN_CENTER_VERTICAL);
        contents->Add(nav, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        list_->AppendColumn("Name", wxLIST_FORMAT_LEFT, FromDIP(360));
        list_->AppendColumn("Size", wxLIST_FORMAT_RIGHT, FromDIP(100));
        list_->AppendColumn("Modified", wxLIST_FORMAT_LEFT, FromDIP(160));
        ui::fill_last_column(list_);
        // GTK draws wxListCtrl itself. Disabling or rebuilding it inside the
        // activation handler faults the process; Windows' native control does not.
        list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) {
            const long row = e.GetIndex();
            CallAfter([this, alive = alive_, row] { if (*alive) OpenRow(row); });
        });
        list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) { if (!rebuilding_) UpdateControls(); });
        list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { if (!rebuilding_) UpdateControls(); });
        list_->Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& e) {
            for (int i = 0; i < list_->GetColumnCount(); ++i) list_->SetColumnWidth(i, e.ScaleX(list_->GetColumnWidth(i)));
            e.Skip();
        });
        contents->Add(list_, 1, wxEXPAND);

        gauge_ = new wxGauge(this, wxID_ANY, 1000, wxDefaultPosition, FromDIP(wxSize(-1, 6)));
        contents->Add(gauge_, 0, wxEXPAND | wxTOP, FromDIP(10));

        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        download_ = ui::button(this, actions, wxS("Download…"), [this] { DownloadRow(Selected()); });
        cancel_ = ui::button(this, actions, "Cancel download", [this] { *cancelled_ = true; cancel_->Disable(); });
        actions->AddStretchSpacer();
        page_label_ = new wxStaticText(this, wxID_ANY, "");
        page_label_->SetForegroundColour(ui::muted());
        actions->Add(page_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        prev_ = ui::button(this, actions, "Previous", [this] { LoadEntries(path_, page_ - 1); });
        next_ = ui::button(this, actions, "Next", [this] { LoadEntries(path_, page_ + 1); });
        contents->Add(actions, 0, wxEXPAND | wxTOP, FromDIP(10));
        status_ = new ui::Status(this);
        contents->Add(status_, 0, wxEXPAND | wxTOP, FromDIP(10));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(contents, 1, wxEXPAND | wxALL, FromDIP(24));
        SetSizer(outer);
        gauge_->Hide();
        cancel_->Hide();
        UpdateControls();
    }

    ~FilesPage() override { *alive_ = false; *cancelled_ = true; }

    bool Busy() const { return busy_; }

    void Activate() {
        const auto token = account_.token();
        if (token == session_) return;
        session_ = token;
        *cancelled_ = true;
        list_->DeleteAllItems();
        shares_.clear();
        share_.clear();
        path_.clear();
        listing_ = {};
        location_->SetValue("Shares");
        page_label_->SetLabel("");
        pending_ = busy_ && !token.empty();
        if (!busy_ && !token.empty()) LoadShares();
    }

private:
    void Work(std::function<void()> job, std::function<void()> done) {
        const auto token = session_;
        busy_ = true; status_->Info(wxS("Loading…")); UpdateControls();
        std::thread([alive=alive_,job=std::move(job),done=std::move(done),this,token] {
            std::string error; bool unauthorized = false;
            try { job(); }
            catch (const ApiError& e) { error=e.what(); unauthorized=e.status==401; }
            catch (const std::exception& e) { error=e.what(); }
            if (!*alive) return;
            wxTheApp->CallAfter([alive,done,error,unauthorized,this,token] {
                if (!*alive) return;
                if (token != session_ || token != account_.token()) {
                    busy_ = false;
                    if (pending_) { pending_ = false; LoadShares(); }
                    return;
                }
                try {
                    if (error.empty()) { status_->Clear(); done(); }
                    else if (unauthorized) status_->Error(wxS("Your session has expired. Use Account → Reconnect to LOKI to sign in again."));
                    else status_->Error(W(error));
                } catch (const std::exception& e) { status_->Error(W(e.what())); }
                busy_ = false; gauge_->Hide(); cancel_->Hide();
                UpdateControls(); Layout();
            });
        }).detach();
    }
    FilesApi Api() const { return account_.files_api(); }
    void LoadShares() {
        auto result=std::make_shared<std::vector<Share>>(); auto api=Api();
        Work([api,result]() mutable { api.discover(); *result=api.shares(); },[this,result] {
            Hold hold(rebuilding_);
            share_.clear(); path_.clear(); shares_=*result; listing_={};
            location_->SetValue(W(account_.server()+" / Shares"));
            list_->DeleteAllItems();
            for (size_t i=0;i<shares_.size();++i) list_->InsertItem(long(i),W(shares_[i].name));
            page_label_->SetLabel("");
            if (shares_.empty()) status_->Info("Nothing is shared with your account yet.");
        });
    }
    void LoadEntries(const std::string& path, int page) {
        auto result=std::make_shared<ShareListing>(); auto api=Api(); const auto share=share_;
        Work([api,result,share,path,page]() mutable { *result=api.entries(share,path,page); },[this,result,path,page] {
            Hold hold(rebuilding_);
            path_=path; page_=page; listing_=*result;
            location_->SetValue(W(share_name_+" / "+path_));
            list_->DeleteAllItems();
            for (size_t i=0;i<listing_.entries.size();++i) {
                const auto& e=listing_.entries[i];
                const auto row=list_->InsertItem(long(i),W(e.name)+(e.directory ? "/" : ""));
                if (row<0) continue;
                list_->SetItem(row,1,e.directory ? wxString("Folder") : size_text(e.size));
                if (e.modified>0) {
                    wxDateTime modified(time_t(e.modified));
                    if (modified.IsValid()) list_->SetItem(row,2,modified.Format("%Y-%m-%d %H:%M"));
                }
            }
            const auto pages = std::max<std::int64_t>(1, (listing_.total + 99) / 100);
            page_label_->SetLabel(pages > 1
                ? wxString::Format(wxS("%lld items · page %d of %lld"), static_cast<long long>(listing_.total), page_ + 1, static_cast<long long>(pages))
                : wxString::Format(listing_.total == 1 ? wxS("%lld item") : wxS("%lld items"), static_cast<long long>(listing_.total)));
            if (listing_.entries.empty()) status_->Info("This folder is empty.");
            if (page_==999 && listing_.total>100000) status_->Info("This folder has more items than the server lets you browse.");
        });
    }
    long Selected() const { return list_->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED); }
    void UpdateControls() {
        list_->Enable(!busy_); refresh_->Enable(!busy_);
        up_->Enable(!busy_&&!share_.empty());
        // Paging only appears for folders with more than one page.
        const bool paged = !share_.empty() && listing_.total > 100;
        if (prev_->IsShown() != paged) { prev_->Show(paged); next_->Show(paged); Layout(); }
        prev_->Enable(!busy_&&!share_.empty()&&page_>0);
        next_->Enable(!busy_&&!share_.empty()&&page_<999&&(std::int64_t(page_)+1)*100<listing_.total);
        const auto row=Selected();
        const bool entry=!share_.empty()&&row>=0&&size_t(row)<listing_.entries.size();
        download_->Enable(!busy_&&entry);
        download_->SetLabel(entry && listing_.entries[size_t(row)].directory ? wxS("Download folder…") : wxS("Download…"));
    }
    void OpenRow(long row) {
        if (busy_ || rebuilding_ || row<0) return;
        if (share_.empty()) {
            if (size_t(row)>=shares_.size()) return;
            share_=shares_[size_t(row)].id; share_name_=shares_[size_t(row)].name; LoadEntries("",0); return;
        }
        if (size_t(row)>=listing_.entries.size()) return;
        const auto entry=listing_.entries[size_t(row)];
        const auto path=path_.empty() ? entry.name : path_+"/"+entry.name;
        if (entry.directory) { LoadEntries(path,0); return; }
        DownloadRow(row);
    }

    void DownloadRow(long row) {
        if (busy_ || rebuilding_ || share_.empty() || row < 0 || size_t(row) >= listing_.entries.size()) return;
        const auto entry = listing_.entries[size_t(row)];
        const auto source = path_.empty() ? entry.name : path_ + "/" + entry.name;
        const auto selected = std::ranges::find(shares_, share_, &Share::id);
        if (selected == shares_.end()) return;
        auto share = *selected;
        fs::path destination;
        auto name = W(entry.name);
        if (entry.directory) {
            fs::path leaf;
            try {
                leaf = safe_download_path(entry.name);
                if (leaf.has_parent_path()) throw std::runtime_error("Invalid folder name.");
            } catch (const std::exception& error) {
                status_->Error(W(error.what()));
                return;
            }
            wxDirDialog save(this, "Choose where to save the downloaded folder", {}, wxDD_DIR_MUST_EXIST);
            if (save.ShowModal() != wxID_OK) return;
            destination = path_from_utf8(U(save.GetPath())) / leaf;
        } else {
            for (auto c : {wxS("/"),wxS("\\"),wxS(":"),wxS("<"),wxS(">"),wxS("\""),wxS("|"),wxS("?"),wxS("*"),wxS("\r"),wxS("\n")})
                name.Replace(c,"_");
            wxFileDialog save(this, "Save downloaded file", "", name, "All files (*.*)|*.*", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            if (save.ShowModal() != wxID_OK) return;
            destination = path_from_utf8(U(save.GetPath()));
        }
        const auto profile = account_.directory();
        const auto session = session_;
        const auto alive = alive_;
        auto cancel=cancelled_=std::make_shared<std::atomic<bool>>(false);
        auto* dispatcher=wxTheApp;
        Work([profile,session,share,source,entry,destination,alive,cancel,dispatcher,this] {
            Store store(profile);
            if (store.token() != session) throw std::runtime_error("The signed-in account changed.");
            auto last=std::chrono::steady_clock::time_point{};
            FileDownloads(store).download(share, source, entry.directory, destination, [&](std::int64_t received,std::int64_t total) {
                if (!*alive||*cancel) return false;
                if (received < 0 || total == 0) return true;
                const auto now=std::chrono::steady_clock::now();
                if (now-last>std::chrono::milliseconds(150)) {
                    last=now;
                    dispatcher->CallAfter([alive,received,total,this] {
                        if (!*alive) return;
                        gauge_->Show(); cancel_->Show();
                        if (total>0) gauge_->SetValue(int(1000.0*double(received)/double(total))); else gauge_->Pulse();
                        status_->Info(total>0 ? "Downloading "+size_text(received)+" of "+size_text(total) : wxString(wxS("Preparing download…")));
                        Layout();
                    });
                }
                return true;
            });
        },[this,name] { status_->Success("Downloaded "+name+" and verified its checksum."); });
        cancel_->Enable(); cancel_->Show(); gauge_->Show(); status_->Info(wxS("Preparing download…")); Layout();
    }
    Store& account_;
    std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> cancelled_=std::make_shared<std::atomic<bool>>(false);
    bool busy_=false, rebuilding_=false, pending_=false;
    std::string session_;
    wxTextCtrl* location_;
    wxButton *up_,*refresh_,*prev_,*next_,*download_,*cancel_;
    ui::Status* status_; wxStaticText* page_label_;
    wxListCtrl* list_; wxGauge* gauge_;
    std::vector<Share> shares_; ShareListing listing_;
    std::string share_,share_name_,path_; int page_=0;
};
}
wxPanel* build_file_browser(wxWindow* parent, Store& store) { return new FilesPage(parent, store); }
void activate_file_browser(wxPanel* panel) { static_cast<FilesPage*>(panel)->Activate(); }
bool file_browser_busy(const wxPanel* panel) { return static_cast<const FilesPage*>(panel)->Busy(); }
} // namespace loki
