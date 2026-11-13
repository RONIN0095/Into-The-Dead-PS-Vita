#ifndef NA_HUD_H
#define NA_HUD_H
// Hides the on-screen touch controls (see na_hud.c)
void na_hud_tick(int frame);
// 1 while a level with its touch controls is on screen
int na_hud_in_level(void);
// 1 while the privacy pop-up at the start is up
int na_hud_gdpr_up(void);
#endif
