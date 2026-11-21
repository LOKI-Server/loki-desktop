#pragma once
class wxWindow;
class wxPanel;
namespace loki {
class Store;
wxPanel* build_apps_page(wxWindow* parent, Store& store);
void activate_apps_page(wxPanel* page);
wxPanel* build_repository_page(wxWindow* parent, Store& store);
void activate_repository_page(wxPanel* page);
bool workspace_page_busy(const wxPanel* page);
}
