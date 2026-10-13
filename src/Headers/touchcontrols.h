//
// touchcontrols.h
// Touchscreen controls (iOS). On other platforms, these are no-op stubs.
//

#pragma once

#include <SDL3/SDL.h>

// Order of the rects returned by TouchControls_GetOverlayRects
enum
{
	kTouchOverlay_NextWeapon,
	kTouchOverlay_Radar,
	kTouchOverlay_Pause,
	kTouchOverlay_StickBase,		// only while the stick is held
	kTouchOverlay_StickKnob,
	kTouchOverlay_Back,				// menus only; via TouchControls_GetBackButtonRect
	kTouchOverlay_COUNT
};

void TouchControls_Init(void);
void TouchControls_Reset(void);
void TouchControls_HandleEvent(const SDL_Event* event);
void TouchControls_Update(void);
bool TouchControls_GetNeed(int needID);
int32_t TouchControls_GetStickMagnitude_Fix32(void);
int TouchControls_GetOverlayRects(SDL_FRect* outRects, int maxRects);

// Menus: a "<" button in the top-left corner that sends UIBack.
// Screens where back is meaningful turn it on; it's always hidden in game and during text entry.
void TouchControls_SetBackButtonVisible(bool visible);
bool TouchControls_GetBackButtonRect(SDL_FRect* outRect);
