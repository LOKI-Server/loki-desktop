#pragma once
#include <wx/window.h>
#include "store.hpp"
namespace loki {
void show_file_browser(wxWindow* parent, Store& store, const std::string& suggested_server);
}
