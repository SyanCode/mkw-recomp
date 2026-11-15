// The settings pages (ui/menu/settings_tabs.cpp): the tab bar and the rows of each tab, for the in-game menu and the start
// screen. ImGui's thread only.
#pragma once
#include <vulkan/vulkan.h>

// The players' tabs first; Advanced (testing and troubleshooting) and Hacks last, drawn dim.
enum { TAB_SCREEN, TAB_GRAPHICS, TAB_SOUND, TAB_CONTROLS, TAB_GENERAL, TAB_ONLINE, TAB_ADVANCED, TAB_HACKS, TAB_COUNT };

void settings_list_gpus(VkInstance inst);   // once: the Vulkan devices the Screen tab offers (MKW_GPU_INDEX)
void settings_select(int tab);              // that tab at the next layout
void settings_opened(void);                 // the pages were (re)opened: the first row takes the cursor
bool settings_modal(void);                  // a confirmation or a key capture is open: B and Esc are its own
void settings_tabs(float scale, float bottom);   // the tab bar and the shown page; bottom: height kept below it
void settings_tabs_done(void);              // after the window's own B / Esc handling, each frame
const char *settings_present_name(const char *mode);   // the menu's name of a MKW_VK_PRESENT mode; NULL for another

// The start screen's side of the Online tab: with the executable's folder given, the room's mods can be compared with the
// installed ones, installed from GameBanana and put on for the match (ui/launcher/mods_room.h). In the game they only show.
void settings_set_launcher(const char *exe_dir);
bool settings_play_requested(void);   // the start screen should start the game now (Play with this room's mods); once
bool settings_mods_requested(void);   // ... and open the Mods page on GameBanana's tab (the next missing mod); once
