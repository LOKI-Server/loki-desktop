#include "ui.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>
#include <memory>

namespace loki::ui {
namespace {

// The sidebar shares the dark green of LOKI's dashboard and icon.
const wxColour kSidebar(14, 22, 18);
const wxColour kSidebarText(206, 220, 211);
const wxColour kSidebarMuted(128, 150, 137);
const wxColour kSidebarHover(24, 38, 31);
const wxColour kSidebarSelected(30, 54, 41);
const wxColour kAccent(125, 231, 167);

}  // namespace

wxColour muted() { return {96, 104, 100}; }
wxColour danger() { return {185, 48, 42}; }
wxColour success() { return {28, 122, 72}; }
wxColour page_background() { return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW); }

wxBitmap tree_bitmap(int size, const wxColour& background) {
    wxBitmap bitmap(size, size, 24);
    {
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(background));
        dc.Clear();
        const double s = size / 32.0;
        dc.SetPen(wxPen(kAccent, std::max(2, int(3 * s)), wxPENSTYLE_SOLID));
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

Sidebar::Sidebar(wxWindow* parent, std::vector<wxString> pages) : wxPanel(parent), pages_(std::move(pages)) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(FromDIP(208), -1));
    SetName("Navigation");
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) { Paint(); });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Arrange(); Refresh(); e.Skip(); });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        const int hit = HitTest(e.GetPosition());
        SetCursor(hit == -1 ? wxNullCursor : wxCursor(wxCURSOR_HAND));
        if (hit != hover_) { hover_ = hit; Refresh(); }
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { if (hover_ != -1) { hover_ = -1; Refresh(); } });
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& e) {
        const int hit = HitTest(e.GetPosition());
        if (hit == -2 && on_account) on_account(wxPoint(account_.GetLeft(), account_.GetTop()));
        else if (hit >= 0 && on_select) on_select(hit);
    });
    Arrange();
}

void Sidebar::Select(int index) {
    if (index == selected_) return;
    selected_ = index;
    Refresh();
}

void Sidebar::SetAccount(const wxString& name, const wxString& server) {
    name_ = name;
    server_ = server;
    Refresh();
}

void Sidebar::Arrange() {
    const auto size = GetClientSize();
    const int inset = FromDIP(10), height = FromDIP(36), gap = FromDIP(2);
    rows_.clear();
    int y = FromDIP(74);
    for (size_t i = 0; i < pages_.size(); ++i) {
        rows_.emplace_back(inset, y, size.GetWidth() - 2 * inset, height);
        y += height + gap;
    }
    const int account_height = FromDIP(54);
    account_ = wxRect(inset, size.GetHeight() - account_height - inset, size.GetWidth() - 2 * inset, account_height);
}

int Sidebar::HitTest(wxPoint point) const {
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].Contains(point)) return int(i);
    if (!name_.empty() && account_.Contains(point)) return -2;
    return -1;
}

void Sidebar::Paint() {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(kSidebar));
    dc.Clear();
    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    const double radius = FromDIP(6);
    const auto base = GetFont();

    // LOKI's mark and name.
    const int mark = FromDIP(26);
    gc->DrawBitmap(tree_bitmap(mark, kSidebar), FromDIP(18), FromDIP(22), mark, mark);
    gc->SetFont(base.Scaled(1.35f).Bold(), *wxWHITE);
    double w, h;
    gc->GetTextExtent("LOKI", &w, &h);
    gc->DrawText("LOKI", FromDIP(52), FromDIP(22) + (mark - h) / 2);

    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& r = rows_[i];
        const bool selected = int(i) == selected_;
        if (selected || int(i) == hover_) {
            gc->SetBrush(wxBrush(selected ? kSidebarSelected : kSidebarHover));
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->DrawRoundedRectangle(r.x, r.y, r.width, r.height, radius);
        }
        if (selected) {
            gc->SetBrush(wxBrush(kAccent));
            gc->DrawRoundedRectangle(r.x, r.y + FromDIP(9), FromDIP(3), r.height - FromDIP(18), FromDIP(2));
        }
        gc->SetFont(selected ? base.Bold() : base, selected ? kAccent : kSidebarText);
        gc->GetTextExtent(pages_[i], &w, &h);
        gc->DrawText(pages_[i], r.x + FromDIP(14), r.y + (r.height - h) / 2);
    }

    if (name_.empty()) return;
    // The signed-in account: an initial in a circle, the name, and the server.
    const auto& a = account_;
    if (hover_ == -2) {
        gc->SetBrush(wxBrush(kSidebarHover));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->DrawRoundedRectangle(a.x, a.y, a.width, a.height, radius);
    }
    const int circle = FromDIP(32);
    const double cx = a.x + FromDIP(10), cy = a.y + (a.height - circle) / 2.0;
    gc->SetBrush(wxBrush(kSidebarSelected));
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->DrawEllipse(cx, cy, circle, circle);
    const wxString initial = name_.Left(1).Upper();
    gc->SetFont(base.Bold(), kAccent);
    gc->GetTextExtent(initial, &w, &h);
    gc->DrawText(initial, cx + (circle - w) / 2, cy + (circle - h) / 2);

    const int text_x = int(cx) + circle + FromDIP(10);
    const int text_width = a.GetRight() - text_x - FromDIP(22);
    wxClientDC measure(this);
    measure.SetFont(base.Bold());
    gc->SetFont(base.Bold(), kSidebarText);
    gc->DrawText(wxControl::Ellipsize(name_, measure, wxELLIPSIZE_END, text_width), text_x, a.y + FromDIP(9));
    const auto caption = base.Scaled(0.9f);
    measure.SetFont(caption);
    gc->SetFont(caption, kSidebarMuted);
    gc->DrawText(wxControl::Ellipsize(server_, measure, wxELLIPSIZE_MIDDLE, text_width), text_x, a.y + FromDIP(28));
    // A chevron hints that the account opens a menu.
    gc->SetPen(wxPen(kSidebarMuted, FromDIP(2)));
    const double vx = a.GetRight() - FromDIP(16), vy = a.y + a.height / 2.0;
    gc->StrokeLine(vx - FromDIP(4), vy - FromDIP(2), vx, vy + FromDIP(2));
    gc->StrokeLine(vx, vy + FromDIP(2), vx + FromDIP(4), vy - FromDIP(2));
}

}  // namespace loki::ui
