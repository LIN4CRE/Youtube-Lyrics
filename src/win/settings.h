#pragma once

namespace ytl {

struct App;

// Opens (or focuses) the native settings window. Everything it edits goes
// straight into the same App::Config the tray menu and hotkeys use, so the two
// can never disagree.
void settings_show(App& app);

// Re-reads the config into the open window, if there is one. Called after a
// tray-menu or hotkey change so an open settings window never shows stale
// values.
void settings_refresh(App& app);

}  // namespace ytl
