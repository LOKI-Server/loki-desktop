// LOKI Client for desktop: a small window and a notification-area icon that keep
// folders in step with a LOKI server's Save sync.
#include <wx/wx.h>
// Feature headers check wxUSE_* settings, so they follow wx/wx.h.
#include <wx/snglinst.h>
#include <wx/stdpaths.h>

#include <memory>

#include "main_frame.hpp"

namespace {

class App : public wxApp {
public:
    bool OnInit() override {
        // Keep the established profile directory and instance lock so existing
        // logins survive the rename and an older client cannot run alongside us.
        SetAppName("LOKI Sync");
        SetVendorName("LOKI");
        const std::filesystem::path config = wxStandardPaths::Get().GetUserDataDir().ToStdWstring();
        single_ = std::make_unique<wxSingleInstanceChecker>();
        SetAppName("LOKI Client");
        SetAppDisplayName("LOKI Client");
        if (single_->IsAnotherRunning()) {
            wxMessageBox("LOKI Client is already running. Look for its icon in the notification area.", "LOKI Client");
            return false;
        }
        store_ = std::make_unique<loki::Store>(config);
        auto* frame = new loki::MainFrame(*store_);
        // Started at sign-in with --background: stay in the notification area.
        if (!(argc > 1 && wxString(argv[1]) == "--background" && store_->signed_in())) frame->Show();
        return true;
    }

private:
    std::unique_ptr<wxSingleInstanceChecker> single_;
    std::unique_ptr<loki::Store> store_;
};

}  // namespace

wxIMPLEMENT_APP(App);
