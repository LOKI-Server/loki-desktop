#include "files_dialog.hpp"
#include "files_api.hpp"
#include "desktop.hpp"
#include <wx/wx.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/listctrl.h>
#include <wx/utils.h>
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
        : wxPanel(parent), account_(store), cache_(store.directory() / "downloads") {
        auto* contents = new wxBoxSizer(wxVERTICAL);
        auto* browser = this;
        auto* title = new wxStaticText(this, wxID_ANY, "Browse files");
        title->SetFont(title->GetFont().Scaled(1.6f).Bold());
        contents->Add(title, 0, wxBOTTOM, FromDIP(12));
        auto* nav = new wxBoxSizer(wxHORIZONTAL);
        up_ = new wxButton(browser,wxID_ANY,"Up");
        up_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
            if (share_.empty()) return;
            if (path_.empty()) { LoadShares(); return; }
            const auto slash = path_.find_last_of('/');
            LoadEntries(slash == std::string::npos ? "" : path_.substr(0,slash),0);
        });
        nav->Add(up_,0,wxRIGHT,FromDIP(8));
        location_ = new wxTextCtrl(browser,wxID_ANY,"Shares",wxDefaultPosition,wxDefaultSize,wxTE_READONLY);
        nav->Add(location_,1,wxRIGHT,FromDIP(8));
        refresh_ = new wxButton(browser,wxID_ANY,"Refresh");
        refresh_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { if (share_.empty()) LoadShares(); else LoadEntries(path_,page_); });
        nav->Add(refresh_);
        contents->Add(nav,0,wxEXPAND|wxBOTTOM,FromDIP(8));
        list_ = new wxListCtrl(browser,wxID_ANY,wxDefaultPosition,wxDefaultSize,wxLC_REPORT|wxLC_SINGLE_SEL);
        list_->AppendColumn("Name",wxLIST_FORMAT_LEFT,FromDIP(330));
        list_->AppendColumn("Size",wxLIST_FORMAT_RIGHT,FromDIP(100));
        list_->AppendColumn("Modified",wxLIST_FORMAT_LEFT,FromDIP(160));
        // GTK draws wxListCtrl itself. Disabling or rebuilding it inside the
        // activation handler faults the process; Windows' native control does not.
        list_->Bind(wxEVT_LIST_ITEM_ACTIVATED,[this](wxListEvent& e) {
            const long row=e.GetIndex();
            CallAfter([this,alive=alive_,row] { if (*alive) OpenRow(row); });
        });
        list_->Bind(wxEVT_LIST_ITEM_SELECTED,[this](wxListEvent&) { if (!rebuilding_) UpdateControls(); });
        list_->Bind(wxEVT_LIST_ITEM_DESELECTED,[this](wxListEvent&) { if (!rebuilding_) UpdateControls(); });
        list_->Bind(wxEVT_DPI_CHANGED,[this](wxDPIChangedEvent& e) {
            for (int i=0;i<list_->GetColumnCount();++i) list_->SetColumnWidth(i,e.ScaleX(list_->GetColumnWidth(i)));
            e.Skip();
        });
        contents->Add(list_,1,wxEXPAND|wxBOTTOM,FromDIP(8));
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        prev_ = new wxButton(browser,wxID_ANY,"Previous");
        next_ = new wxButton(browser,wxID_ANY,"Next");
        prev_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { LoadEntries(path_,page_-1); });
        next_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { LoadEntries(path_,page_+1); });
        actions->Add(prev_,0,wxRIGHT,FromDIP(6)); actions->Add(next_,0,wxRIGHT,FromDIP(8));
        page_label_ = new wxStaticText(browser,wxID_ANY,"");
        actions->Add(page_label_,1,wxALIGN_CENTER_VERTICAL);
        download_ = new wxButton(browser,wxID_ANY,wxS("Download…"));
        download_->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) { OpenSelected(); });
        actions->Add(download_,0,wxRIGHT,FromDIP(8));
        contents->Add(actions, 0, wxEXPAND);
        status_ = new wxStaticText(this, wxID_ANY, "");
        contents->Add(status_, 0, wxEXPAND | wxTOP, FromDIP(8));
        gauge_ = new wxGauge(this, wxID_ANY, 1000);
        contents->Add(gauge_, 0, wxEXPAND | wxTOP, FromDIP(8));
        cancel_ = new wxButton(this, wxID_ANY, "Cancel download");
        cancel_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { *cancelled_ = true; cancel_->Disable(); });
        contents->Add(cancel_, 0, wxTOP, FromDIP(8));
        auto* outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(contents, 1, wxEXPAND | wxALL, FromDIP(16));
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
        busy_ = true; status_->SetLabel(wxS("Loading…")); UpdateControls();
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
                    if (unauthorized) { status_->SetLabel("Your account session expired. Sign out and reconnect in the main window."); }
                    if (error.empty()) { status_->SetLabel(""); done(); }
                    else { status_->SetLabel(W(error)); status_->Wrap(GetClientSize().x-FromDIP(32)); }
                } catch (const std::exception& e) { status_->SetLabel(W(e.what())); }
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
            if (shares_.empty()) status_->SetLabel("No shares are available for this account.");
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
            page_label_->SetLabel(wxString::Format(wxS("Page %d · %lld entries"),page_+1,static_cast<long long>(listing_.total)));
            if (listing_.entries.empty()) status_->SetLabel("This folder is empty.");
            if (page_==999 && listing_.total>100000) status_->SetLabel("This folder exceeds the server's browsing limit.");
        });
    }
    long Selected() const { return list_->GetNextItem(-1,wxLIST_NEXT_ALL,wxLIST_STATE_SELECTED); }
    void UpdateControls() {
        list_->Enable(!busy_); refresh_->Enable(!busy_);
        up_->Enable(!busy_&&!share_.empty());
        prev_->Enable(!busy_&&!share_.empty()&&page_>0);
        next_->Enable(!busy_&&!share_.empty()&&page_<999&&(std::int64_t(page_)+1)*100<listing_.total);
        const auto row=Selected();
        const bool file=!share_.empty()&&row>=0&&size_t(row)<listing_.entries.size()&&!listing_.entries[size_t(row)].directory;
        download_->Enable(!busy_&&file);
    }
    void OpenSelected() { OpenRow(Selected()); }
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
        // Server names are only a suggested leaf name, never a local path.
        auto name=W(entry.name); for (auto c : {wxS("/"),wxS("\\"),wxS(":"),wxS("<"),wxS(">"),wxS("\""),wxS("|"),wxS("?"),wxS("*"),wxS("\r"),wxS("\n")}) name.Replace(c,"_");
        wxFileDialog save(this,"Save downloaded file","",name,"All files (*.*)|*.*",wxFD_SAVE|wxFD_OVERWRITE_PROMPT);
        if (save.ShowModal()!=wxID_OK) return;
        const auto destination=path_from_utf8(U(save.GetPath()));
        auto api=Api(); const auto share=share_; const auto cache=cache_; auto alive=alive_;
        auto cancel=cancelled_=std::make_shared<std::atomic<bool>>(false);
        auto* dispatcher=wxTheApp;
        Work([api,share,path,cache,destination,alive,cancel,dispatcher,this]() mutable {
            auto last=std::chrono::steady_clock::time_point{};
            Desktop::download(api,cache,share,path,destination,[&](std::int64_t received,std::int64_t total) {
                if (!*alive||*cancel) return false;
                const auto now=std::chrono::steady_clock::now();
                if (now-last>std::chrono::milliseconds(150)) {
                    last=now;
                    dispatcher->CallAfter([alive,received,total,this] {
                        if (!*alive) return;
                        gauge_->Show(); cancel_->Show();
                        if (total>0) gauge_->SetValue(int(1000.0*double(received)/double(total))); else gauge_->Pulse();
                        status_->SetLabel(total>0 ? "Downloading "+size_text(received)+" of "+size_text(total) : wxString(wxS("Preparing download…")));
                        Layout();
                    });
                }
                return true;
            }, [alive,cancel] { return alive->load() && !cancel->load(); });
        },[this,name] { status_->SetLabel("Downloaded "+name+" and verified its checksum."); });
        cancel_->Enable(); cancel_->Show(); gauge_->Show(); status_->SetLabel(wxS("Preparing download…")); Layout();
    }
    Store& account_; fs::path cache_;
    std::shared_ptr<std::atomic<bool>> alive_=std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> cancelled_=std::make_shared<std::atomic<bool>>(false);
    bool busy_=false, rebuilding_=false, pending_=false;
    std::string session_;
    wxTextCtrl* location_;
    wxButton *up_,*refresh_,*prev_,*next_,*download_,*cancel_;
    wxStaticText *status_,*page_label_;
    wxListCtrl* list_; wxGauge* gauge_;
    std::vector<Share> shares_; ShareListing listing_;
    std::string share_,share_name_,path_; int page_=0;
};
}
wxPanel* build_file_browser(wxWindow* parent, Store& store) { return new FilesPage(parent, store); }
void activate_file_browser(wxPanel* panel) { static_cast<FilesPage*>(panel)->Activate(); }
bool file_browser_busy(const wxPanel* panel) { return static_cast<const FilesPage*>(panel)->Busy(); }
} // namespace loki
