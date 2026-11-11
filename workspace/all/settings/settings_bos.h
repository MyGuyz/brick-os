#ifndef SETTINGS_BOS_H
#define SETTINGS_BOS_H

// Brick OS ตั้งค่า (DESIGN.md 6.6): the whole screen from the menu's page stack.

#include "settings_menu.h"

void SettingsBos_render(SDL_Surface* screen, SettingsPage** stack, int depth);
int SettingsBos_order(const char* category); // the root category's place in the 6.7 groups
bool SettingsBos_isToggle(SettingItem* it);  // an Off/On (or Hide/Show) row, drawn as a switch: A flips it
bool SettingsBos_isPickable(SettingItem* it); // a Left/Right value row: A opens its value list
void SettingsBos_pickerOpen(SettingItem* it);
bool SettingsBos_pickerActive(void);
void SettingsBos_pickerInput(bool* dirty); // while active: Up/Down (L1/R1 a page), A sets, B closes
// ธีมสี (DESIGN §6.8)
void SettingsBos_preset(int i, uint32_t* focus, uint32_t* primary); // the six ธีมสำเร็จรูป
void SettingsBos_themeCards(SDL_Surface* s, SettingItem* it, int x, int y, int w, int h, int hot); // their cards
void SettingsBos_colorSheetOpen(int slot, const char* title); // THEME_FOCUS / PRIMARY / HINT; A sets, B cancels

// ปุ่มลัด's "press the button you want" sheet over the page (DESIGN §6.9); secs = the countdown
void SettingsBos_captureSheet(SDL_Surface* s, const char* name, const char* note, int secs);

// ปุ่มพิเศษ (DESIGN §6.9b): the key's popup (กดคลิก / กดค้าง, then what it does), its light, the pick hand-over
void SettingsBos_keyOpen(int key);   // BOSKEY_*: step 1 of its popup
void SettingsBos_keyReset(int key);  // X on its row: tap and hold back to the defaults
int SettingsBos_keyResume(void);     // at start: a pick the launcher answered; the key to show (its popup open), or -1
bool SettingsBos_keyTick(bool* quit); // every frame: the light's timer, an action keymon left (Settings closes)

#endif
