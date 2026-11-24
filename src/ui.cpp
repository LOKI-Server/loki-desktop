#include "ui.hpp"

#include <wx/bmpbndl.h>
#include <wx/imaglist.h>
#include <wx/statline.h>

#include <algorithm>
#include <memory>

namespace loki::ui {
namespace {

const wxColour kAccent(125, 231, 167);
const wxColour kLogoGreen(28, 122, 72);

// Sidebar icons from LOKI's web dashboard (static/lib/icons.js), stroked on a 24px grid.
const char* const kSyncIcon = "M4 10a8 8 0 0 1 14.5-4.5L20 7M20 3v4h-4M20 14a8 8 0 0 1-14.5 4.5L4 17M4 21v-4h4M9 12l2 2 4-4";
const char* const kFolderIcon = "M3 6.5A1.5 1.5 0 0 1 4.5 5H9l2 2.5h8.5A1.5 1.5 0 0 1 21 9v9.5a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5z";
const char* const kUpdatesIcon = "M20 7v5h-5M20 12a8 8 0 0 0-14-5M4 17v-5h5M4 12a8 8 0 0 0 14 5";
const char* const kRepoIcon = "M5 4a2 2 0 0 1 2-2h12v16H7a2 2 0 0 0-2 2zM5 20a2 2 0 0 0 2 2h12v-4M9 6h6";

/** A dashboard icon drawn in `colour`, `size` pixels square, inside a `width` by `height` transparent cell. */
wxBitmap stroke_icon(const char* path, const wxColour& colour, int size, int width, int height) {
    const auto svg = wxString::Format(
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="%s" stroke-width="1.75" )"
        R"(stroke-linecap="round" stroke-linejoin="round"><path d="%s"/></svg>)",
        colour.GetAsString(wxC2S_HTML_SYNTAX), path);
    const auto icon = wxBitmapBundle::FromSVG(svg.utf8_str(), wxSize(size, size)).GetBitmap(wxSize(size, size));
    wxImage cell(width, height);
    cell.SetAlpha();
    std::fill_n(cell.GetAlpha(), width * height, 0);
    cell.Paste(icon.ConvertToImage(), (width - size) / 2, (height - size) / 2, wxIMAGE_ALPHA_BLEND_COMPOSE);
    return wxBitmap(cell);
}

}  // namespace

wxColour muted() { return {96, 104, 100}; }
wxColour danger() { return {185, 48, 42}; }
wxColour success() { return {28, 122, 72}; }
wxColour page_background() { return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW); }

wxBitmap tree_bitmap(int size, const wxColour& background, const wxColour& line) {
    wxBitmap bitmap(size, size, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(background));
        dc.Clear();
        const double s = size / 32.0;
        dc.SetPen(wxPen(line.IsOk() ? line : kAccent, std::max(2, int(3 * s)), wxPENSTYLE_SOLID));
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
    return bitmap;
}

wxIcon tree_icon(int size) {
    wxIcon icon;
    icon.CopyFromBitmap(tree_bitmap(size, wxColour(8, 14, 11)));
    return icon;
}

Header page_header(wxWindow* page, const wxString& title, const wxString& subtitle) {
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* titles = new wxBoxSizer(wxVERTICAL);
    auto* heading = new wxStaticText(page, wxID_ANY, title);
    heading->SetFont(heading->GetFont().Scaled(1.6f).Bold());
    titles->Add(heading);
    auto* below = new wxStaticText(page, wxID_ANY, subtitle);
    below->SetForegroundColour(muted());
    titles->Add(below, 0, wxTOP, page->FromDIP(2));
    row->Add(titles, 1, wxALIGN_CENTER_VERTICAL);
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    row->Add(actions, 0, wxALIGN_CENTER_VERTICAL);
    return {row, below, actions};
}

wxButton* button(wxWindow* parent, wxSizer* row, const wxString& label, std::function<void()> action) {
    auto* b = new wxButton(parent, wxID_ANY, label);
    b->Bind(wxEVT_BUTTON, [action = std::move(action)](wxCommandEvent&) { action(); });
    if (row->GetItemCount()) row->AddSpacer(parent->FromDIP(6));
    row->Add(b, 0, wxALIGN_CENTER_VERTICAL);
    return b;
}

wxStaticText* paragraph(wxWindow* parent, const wxString& text, const wxColour& colour) {
    auto* label = new Status(parent);
    label->Info(text);
    if (colour.IsOk()) label->SetForegroundColour(colour);
    return label;
}

Status::Status(wxWindow* parent) : wxStaticText(parent, wxID_ANY, "") {
    SetMinSize(wxSize(FromDIP(40), 0));
    // Rewrap whenever the sizer gives the line a different width.
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        e.Skip();
        if (e.GetSize().GetWidth() == width_) return;
        width_ = e.GetSize().GetWidth();
        if (Rewrap()) CallAfter([this] { if (GetParent()) GetParent()->Layout(); });
    });
}

void Status::Info(const wxString& text) { Show(text, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT)); }
void Status::Error(const wxString& text) { Show(text, danger()); }
void Status::Success(const wxString& text) { Show(text, success()); }

void Status::Show(const wxString& text, const wxColour& colour) {
    text_ = text;
    SetForegroundColour(colour);
    Rewrap();
    if (GetParent()) GetParent()->Layout();
}

bool Status::Rewrap() {
    SetLabel(text_);
    // Before the first layout the width is unknown; wrap once it is.
    if (width_ > FromDIP(40) && !text_.empty()) Wrap(width_);
    const int lines = text_.empty() ? 0 : int(GetLabel().Freq(wxUniChar(10))) + 1;
    const wxSize size(FromDIP(40), lines * GetCharHeight());
    if (size == GetMinSize()) return false;
    SetMinSize(size);
    return true;
}

void fill_last_column(wxListCtrl* list) {
    const auto fit = [list] {
        const int last = list->GetColumnCount() - 1;
        if (last < 0) return;
        int used = 0;
        for (int i = 0; i < last; ++i) used += list->GetColumnWidth(i);
        const int width = list->GetClientSize().GetWidth() - used;
        list->SetColumnWidth(last, std::max(list->FromDIP(80), width));
    };
    list->Bind(wxEVT_SIZE, [list, fit](wxSizeEvent& e) {
        e.Skip();
        list->CallAfter(fit);
    });
    list->Bind(wxEVT_LIST_COL_END_DRAG, [list, fit](wxListEvent& e) {
        e.Skip();
        list->CallAfter(fit);
    });
}

Sidebar::Sidebar(wxWindow* parent, std::vector<wxString> pages) : wxPanel(parent) {
    const auto background = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);
    SetBackgroundColour(background);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* column = new wxBoxSizer(wxVERTICAL);

    auto* brand = new wxBoxSizer(wxHORIZONTAL);
    brand->Add(new wxStaticBitmap(this, wxID_ANY, tree_bitmap(FromDIP(24), background, kLogoGreen)), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* name = new wxStaticText(this, wxID_ANY, "LOKI");
    name->SetFont(name->GetFont().Scaled(1.3f).Bold());
    brand->Add(name, 0, wxALIGN_CENTER_VERTICAL);
    column->Add(brand, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(16));

    // A plain Windows list in Explorer's style, one row per page with the dashboard's icon for it.
    nav_ = new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                          wxLC_LIST | wxLC_SINGLE_SEL | wxBORDER_NONE);
    nav_->EnableSystemTheme();
    nav_->SetBackgroundColour(background);
    nav_->SetFont(nav_->GetFont().Scaled(1.05f));
    // Grey like the dashboard's sidebar, and green for the open page.
    const char* const icons[] = {kSyncIcon, kFolderIcon, kUpdatesIcon, kRepoIcon};
    const int size = FromDIP(18), width = FromDIP(30), height = FromDIP(36);
    images_ = std::make_unique<wxImageList>(width, height, false);
    for (const auto& colour : {wxColour(95, 122, 105), kLogoGreen})
        for (const auto* icon : icons) images_->Add(stroke_icon(icon, colour, size, width, height));
    nav_->SetImageList(images_.get(), wxIMAGE_LIST_SMALL);
    for (size_t i = 0; i < pages.size(); ++i) nav_->InsertItem(long(i), pages[i], int(i % std::size(icons)));
    // List view lays items out in columns; one column as wide as the list keeps them in a single stack.
    nav_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        e.Skip();
        nav_->CallAfter([this] { nav_->SetColumnWidth(0, nav_->GetClientSize().GetWidth() - FromDIP(4)); });
    });
    nav_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) {
        if (quiet_ || e.GetIndex() == selected_) return;
        Select(int(e.GetIndex()), true);
        if (on_select) on_select(selected_);
    });
    // Clicking empty space would leave no page selected; keep the current one.
    nav_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) {
        if (!quiet_) CallAfter([this] { Select(selected_, true); });
    });
    column->Add(nav_, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));

    // The signed-in account, with its commands behind a button.
    column->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    name_ = new wxStaticText(this, wxID_ANY, "");
    name_->SetFont(name_->GetFont().Bold());
    column->Add(name_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    // Sized by the sidebar rather than its text; long addresses are shortened in the middle to fit.
    server_ = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
    server_->SetForegroundColour(muted());
    server_->SetMinSize(wxSize(FromDIP(40), server_->GetCharHeight()));
    column->AddSpacer(FromDIP(2));
    column->Add(server_, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
    server_->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
        e.Skip();
        const auto fitted = wxControl::Ellipsize(server_text_, wxClientDC(server_), wxELLIPSIZE_MIDDLE, e.GetSize().GetWidth());
        if (fitted != server_->GetLabel()) server_->SetLabel(fitted);
    });
    auto* account = new wxButton(this, wxID_ANY, wxS("Account ▾"));
    account->Bind(wxEVT_BUTTON, [this, account](wxCommandEvent&) {
        if (on_account) on_account(account->GetPosition() + wxPoint(0, account->GetSize().GetHeight()));
    });
    column->Add(account, 0, wxALL, FromDIP(12));

    row->Add(column, 1, wxEXPAND);
    row->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL), 0, wxEXPAND);
    SetSizer(row);
    SetMinSize(wxSize(FromDIP(200), -1));
    Select(0, true);
}

Sidebar::~Sidebar() = default;

void Sidebar::Select(int index) { Select(index, false); }

void Sidebar::Select(int index, bool force) {
    if (index < 0 || index >= nav_->GetItemCount() || (index == selected_ && !force)) return;
    // Only the open page's icon is green.
    if (selected_ >= 0) nav_->SetItemImage(selected_, selected_ % 4);
    nav_->SetItemImage(index, index % 4 + 4);
    selected_ = index;
    quiet_ = true;
    nav_->Select(index);
    nav_->Focus(index);
    quiet_ = false;
}

void Sidebar::SetAccount(const wxString& name, const wxString& server) {
    name_->SetLabel(name);
    server_text_ = server;
    server_->SetLabel(wxControl::Ellipsize(server, wxClientDC(server_), wxELLIPSIZE_MIDDLE,
                                           std::max(FromDIP(40), server_->GetSize().GetWidth())));
    server_->SetToolTip(server);
    Layout();
}

}  // namespace loki::ui
