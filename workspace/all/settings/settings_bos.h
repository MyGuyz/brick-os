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

#endif
