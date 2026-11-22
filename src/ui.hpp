#pragma once
// Shared pieces of the desktop window: colours, page headers, the status line,
// and the sidebar. Pages build from these so they look and behave alike.
#include <wx/wx.h>
#include <wx/listctrl.h>

#include <functional>
#include <memory>
#include <vector>

namespace loki::ui {

wxColour muted();
wxColour danger();
wxColour success();
wxColour page_background();

/** LOKI's world-tree glyph, drawn rather than shipped as a file, in LOKI green unless `line` is given. */
wxBitmap tree_bitmap(int size, const wxColour& background, const wxColour& line = {});
wxIcon tree_icon(int size);

/** A page title with a muted subtitle beneath it and the page's main actions on the right. */
struct Header {
    wxBoxSizer* sizer;
    wxStaticText* subtitle;
    wxBoxSizer* actions;
};
Header page_header(wxWindow* page, const wxString& title, const wxString& subtitle = {});

/** A button that runs `action`, added to `row` with the usual gap. */
wxButton* button(wxWindow* parent, wxSizer* row, const wxString& label, std::function<void()> action);

/** Text that wraps to the available width. */
wxStaticText* paragraph(wxWindow* parent, const wxString& text, const wxColour& colour = {});

/** The line under a page saying what is happening or what went wrong. Errors show in red. */
class Status : public wxStaticText {
public:
    explicit Status(wxWindow* parent);
    void Info(const wxString& text);
    void Error(const wxString& text);
    void Success(const wxString& text);
    void Clear() { Info({}); }

private:
    void Show(const wxString& text, const wxColour& colour);
    // Wraps the text to the current width; true when the needed height changed.
    bool Rewrap();
    wxString text_;
    int width_ = 0;
};

/** Keeps the last column of a report list filling the remaining width. */
void fill_last_column(wxListCtrl* list);

/**
 * The navigation column: LOKI's name, a native list with one row per page, and
 * the signed-in account, whose Account button calls `on_account`.
 */
class Sidebar : public wxPanel {
public:
    Sidebar(wxWindow* parent, std::vector<wxString> pages);
    ~Sidebar() override;
    void Select(int index);
    void SetAccount(const wxString& name, const wxString& server);
    std::function<void(int)> on_select;
    // Receives where to open the account menu, in sidebar coordinates.
    std::function<void(wxPoint)> on_account;

private:
    void Select(int index, bool force);

    wxListView* nav_;
    std::unique_ptr<wxImageList> images_;
    wxStaticText *name_, *server_;
    wxString server_text_;
    int selected_ = -1;
    // Set while the sidebar changes the selection itself, so it does not report it.
    bool quiet_ = false;
};

}  // namespace loki::ui
