#pragma once
class wxWindow;
class wxPanel;
namespace loki {
class Store;
wxPanel* build_apps_page(wxWindow* parent, Store& store);
void activate_apps_page(wxPanel* page);
wxPanel* build_git_page(wxWindow* parent, Store& store);
void activate_git_page(wxPanel* page);
}
