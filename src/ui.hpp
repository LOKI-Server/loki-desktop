#pragma once
// Shared pieces of the desktop window: colours, page headers, the status line,
// and the sidebar. Pages build from these so they look and behave alike.
#include <wx/wx.h>
#include <wx/listctrl.h>

#include <functional>
#include <vector>

namespace loki::ui {

wxColour muted();
wxColour danger();
wxColour success();
wxColour page_background();

/** LOKI's world-tree glyph, drawn rather than shipped as a file. */
wxBitmap tree_bitmap(int size, const wxColour& background);
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
 * The dark navigation column: LOKI's mark, one row per page, and the signed-in
 * account at the bottom. Clicking the account opens `account_menu`.
 */
class Sidebar : public wxPanel {
public:
    Sidebar(wxWindow* parent, std::vector<wxString> pages);
    void Select(int index);
    void SetAccount(const wxString& name, const wxString& server);
    std::function<void(int)> on_select;
    std::function<void(wxPoint)> on_account;

private:
    void Paint();
    void Arrange();
    int HitTest(wxPoint point) const;

    std::vector<wxString> pages_;
    std::vector<wxRect> rows_;
    wxRect account_;
    wxString name_, server_;
    int selected_ = 0;
    int hover_ = -1;  // A page row, or -2 for the account.
};

}  // namespace loki::ui
