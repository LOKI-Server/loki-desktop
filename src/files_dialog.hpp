#pragma once
class wxWindow;
class wxPanel;
namespace loki {
class Store;
wxPanel* build_file_browser(wxWindow* parent, Store& store);
void activate_file_browser(wxPanel* panel);
}
