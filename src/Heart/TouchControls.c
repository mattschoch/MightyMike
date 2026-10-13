/****************************/
/*   	TOUCHCONTROLS.C	    */
/* iOS touchscreen controls */
/****************************/

//
// Touchscreen controls for iOS. Touches are turned into control "needs"
// that Input.c merges with the keyboard/mouse/gamepad, so gameplay code
// doesn't need to know about touch at all.
//
// MENUS (not in game, or game paused):
//   - tap (1 finger, no drag)	-> UIConfirm
//   - 2-finger tap				-> UIBack + UIPause
//   - while text input is active (high-score name entry), any tap also -> UIPause
//   - swipe left/right/up/down	-> UILeft/UIRight/UIUp/UIDown
//     (keep dragging to step again)
//   - top-left "<" button		-> UIBack (only on screens that opt in, see TouchControls_SetBackButtonVisible)
//
// GAMEPLAY:
//   - left half: floating virtual stick -> Up/Down/Left/Right + analog magnitude
//   - right half: hold -> Attack (+ UIConfirm, like the space bar, to dismiss the radar)
//   - top-right strip: [NextWeapon][Radar][Pause] buttons
//


/***************/
/* EXTERNALS   */
/***************/

#include <Pomme.h>
#include <SDL3/SDL.h>
#include <math.h>

#include "misc.h"
#include "input.h"
#include "touchcontrols.h"
#include "structures.h"
#include "externs.h"

#ifdef SDL_PLATFORM_IOS

/****************************/
/*    CONSTANTS             */
/****************************/

#define MAX_FINGERS				10

#define STICK_RADIUS			0.10f			// fraction of window height
#define STICK_DEADZONE			0.20f			// fraction of stick radius
#define STICK_8WAY_THRESHOLD	0.38f			// ~sin(22.5 deg)
#define SWIPE_DISTANCE			0.08f			// fraction of window height

#define ZONE_TOP				0.0f
#define ZONE_HEIGHT				0.16f
#define ZONE_WIDTH				0.11f			// fraction of window width

enum
{
	kRole_None,
	kRole_UI,
	kRole_Stick,
	kRole_Attack,
	kRole_Button,
};

typedef struct
{
	bool			active;
	SDL_FingerID	id;
	int				role;
	float			x, y;						// current position (normalized)
	float			anchorX, anchorY;			// stick origin / swipe start (normalized)
} TouchFinger;

static const int kZoneNeeds[] = { kNeed_NextWeapon, kNeed_Radar, kNeed_UIPause };	// left to right
#define NUM_ZONES ((int)(sizeof(kZoneNeeds) / sizeof(kZoneNeeds[0])))

_Static_assert(NUM_ZONES == kTouchOverlay_StickBase, "overlay rect order must match touchcontrols.h");


/**********************/
/*     VARIABLES      */
/**********************/

static TouchFinger	gFingers[MAX_FINGERS];

static bool			gTouchPendingPulse[NUM_CONTROL_NEEDS];	// set by events, consumed by next update
static bool			gTouchFramePulse[NUM_CONTROL_NEEDS];	// one-frame presses for this update
static bool			gTouchHeld[NUM_CONTROL_NEEDS];			// continuous presses for this update
static int32_t		gTouchStickMagnitude = 0;

static int			gUIGestureMaxFingers = 0;				// max simultaneous fingers in current UI gesture
static bool			gUIGestureMoved = false;				// current UI gesture swiped (so it isn't a tap)

static bool			gBackButtonWanted = false;				// set by menu screens where back does something


/**********************/
/* STATIC FUNCTIONS   */
/**********************/

static bool IsGameplayMode(void)
{
	return gIsInGame && !gIsGamePaused;
}

static void GetWindowSizeF(float* w, float* h)
{
	int wi = 1;
	int hi = 1;
	if (gSDLWindow)
		SDL_GetWindowSize(gSDLWindow, &wi, &hi);
	*w = (float) (wi > 0 ? wi : 1);
	*h = (float) (hi > 0 ? hi : 1);
}

static SDL_FRect GetZoneRect(int zone)
{
	SDL_FRect r;
	r.w = ZONE_WIDTH;
	r.h = ZONE_HEIGHT;
	r.x = 1.0f - (NUM_ZONES - zone) * ZONE_WIDTH;
	r.y = ZONE_TOP;
	return r;
}

static int FindZoneAt(float x, float y)
{
	for (int i = 0; i < NUM_ZONES; i++)
	{
		SDL_FRect r = GetZoneRect(i);
		if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
			return i;
	}
	return -1;
}

static bool IsBackButtonVisible(void)
{
	return gBackButtonWanted
		&& !gIsInGame												// in game (incl. paused) there's the Pause button
		&& !(gSDLWindow && SDL_TextInputActive(gSDLWindow));		// keyboard up: high-score name entry
}

static SDL_FRect GetBackButtonRect(void)
{
	return (SDL_FRect) { 0.0f, ZONE_TOP, ZONE_WIDTH, ZONE_HEIGHT };	// mirror of the Pause zone
}

static bool IsInBackButton(float x, float y)
{
	SDL_FRect r = GetBackButtonRect();
	return IsBackButtonVisible() && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static TouchFinger* FindFinger(SDL_FingerID id)
{
	for (int i = 0; i < MAX_FINGERS; i++)
	{
		if (gFingers[i].active && gFingers[i].id == id)
			return &gFingers[i];
	}
	return NULL;
}

static TouchFinger* AllocFinger(SDL_FingerID id)
{
	for (int i = 0; i < MAX_FINGERS; i++)
	{
		if (!gFingers[i].active)
		{
			SDL_zero(gFingers[i]);
			gFingers[i].active = true;
			gFingers[i].id = id;
			return &gFingers[i];
		}
	}
	return NULL;
}

static int CountFingersWithRole(int role)
{
	int n = 0;
	for (int i = 0; i < MAX_FINGERS; i++)
	{
		if (gFingers[i].active && gFingers[i].role == role)
			n++;
	}
	return n;
}

static TouchFinger* GetStickFinger(void)
{
	for (int i = 0; i < MAX_FINGERS; i++)
	{
		if (gFingers[i].active && gFingers[i].role == kRole_Stick)
			return &gFingers[i];
	}
	return NULL;
}

/********************** FINGER DOWN ***************************/

static void OnFingerDown(const SDL_TouchFingerEvent* e)
{
	TouchFinger* f = FindFinger(e->fingerID);
	if (!f)
		f = AllocFinger(e->fingerID);
	if (!f)
		return;

	f->x = f->anchorX = e->x;
	f->y = f->anchorY = e->y;

	if (!IsGameplayMode())
	{
		if (IsInBackButton(e->x, e->y))				// on-screen back button: not part of a tap/swipe gesture
		{
			f->role = kRole_Button;
			gTouchPendingPulse[kNeed_UIBack] = true;
			return;
		}

		f->role = kRole_UI;

		int numUIFingers = CountFingersWithRole(kRole_UI);
		if (numUIFingers == 1)						// start new gesture
		{
			gUIGestureMaxFingers = 1;
			gUIGestureMoved = false;
		}
		else if (numUIFingers > gUIGestureMaxFingers)
		{
			gUIGestureMaxFingers = numUIFingers;
		}
		return;
	}

	int zone = FindZoneAt(e->x, e->y);
	if (zone >= 0)									// on-screen button
	{
		f->role = kRole_Button;
		gTouchPendingPulse[kZoneNeeds[zone]] = true;
	}
	else if (e->x < 0.5f)							// left half: stick (only one)
	{
		f->role = GetStickFinger() ? kRole_None : kRole_Stick;
	}
	else											// right half: attack
	{
		f->role = kRole_Attack;
	}
}

/********************** FINGER MOTION ***************************/

static void OnFingerMotion(const SDL_TouchFingerEvent* e)
{
	TouchFinger* f = FindFinger(e->fingerID);
	if (!f)
		return;

	f->x = e->x;
	f->y = e->y;

	float w, h;
	GetWindowSizeF(&w, &h);
	float dx = (f->x - f->anchorX) * w;
	float dy = (f->y - f->anchorY) * h;
	float dist = sqrtf(dx*dx + dy*dy);

	switch (f->role)
	{
		case kRole_UI:
		{
			if (gUIGestureMaxFingers > 1)				// multi-finger gestures don't swipe
				break;

			if (dist < SWIPE_DISTANCE * h)
				break;

			if (fabsf(dx) > fabsf(dy))
				gTouchPendingPulse[dx < 0 ? kNeed_UILeft : kNeed_UIRight] = true;
			else
				gTouchPendingPulse[dy < 0 ? kNeed_UIUp : kNeed_UIDown] = true;

			gUIGestureMoved = true;
			f->anchorX = f->x;							// re-arm: keep dragging to step again
			f->anchorY = f->y;
			break;
		}

		case kRole_Stick:
		{
			float radius = STICK_RADIUS * h;
			if (dist > radius)							// floating stick: drag origin along
			{
				float excess = (dist - radius) / dist;
				f->anchorX += (dx * excess) / w;
				f->anchorY += (dy * excess) / h;
			}
			break;
		}

		default:
			break;
	}
}

/********************** FINGER UP ***************************/

static void OnFingerUp(const SDL_TouchFingerEvent* e, bool canceled)
{
	TouchFinger* f = FindFinger(e->fingerID);
	if (!f)
		return;

	int role = f->role;
	f->active = false;

	if (role == kRole_UI
		&& !canceled
		&& CountFingersWithRole(kRole_UI) == 0			// last finger of the gesture lifted
		&& !gUIGestureMoved
		&& !IsGameplayMode())
	{
		if (gUIGestureMaxFingers >= 2)
		{
			gTouchPendingPulse[kNeed_UIBack] = true;
			gTouchPendingPulse[kNeed_UIPause] = true;
		}
		else
		{
			gTouchPendingPulse[kNeed_UIConfirm] = true;
		}

		if (gSDLWindow && SDL_TextInputActive(gSDLWindow))	// high-score name entry: any tap outside the keyboard finishes
			gTouchPendingPulse[kNeed_UIPause] = true;
	}
}

#pragma mark -

/************************* PUBLIC API *********************************/

void TouchControls_Init(void)
{
	// Don't synthesize mouse events from touches: kNeed_Attack is bound to the
	// left mouse button, so any touch (incl. the stick) would also fire.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

	TouchControls_Reset();
}

void TouchControls_Reset(void)
{
	SDL_zeroa(gFingers);
	SDL_zeroa(gTouchPendingPulse);
	SDL_zeroa(gTouchFramePulse);
	SDL_zeroa(gTouchHeld);
	gTouchStickMagnitude = 0;
	gUIGestureMaxFingers = 0;
	gUIGestureMoved = false;
}

void TouchControls_HandleEvent(const SDL_Event* event)
{
	switch (event->type)
	{
		case SDL_EVENT_FINGER_DOWN:
			OnFingerDown(&event->tfinger);
			break;

		case SDL_EVENT_FINGER_MOTION:
			OnFingerMotion(&event->tfinger);
			break;

		case SDL_EVENT_FINGER_UP:
			OnFingerUp(&event->tfinger, false);
			break;

		case SDL_EVENT_FINGER_CANCELED:
			OnFingerUp(&event->tfinger, true);
			break;

		default:
			break;
	}
}

void TouchControls_Update(void)
{
	// Latch one-frame presses produced by events since the last update
	SDL_memcpy(gTouchFramePulse, gTouchPendingPulse, sizeof(gTouchFramePulse));
	SDL_zeroa(gTouchPendingPulse);

	SDL_zeroa(gTouchHeld);
	gTouchStickMagnitude = 0;

	if (!IsGameplayMode())
		return;

	// Attack
	if (CountFingersWithRole(kRole_Attack) > 0)
	{
		gTouchHeld[kNeed_Attack] = true;
		gTouchHeld[kNeed_UIConfirm] = true;		// like the space bar (dismisses radar)
	}

	// Virtual stick
	TouchFinger* f = GetStickFinger();
	if (f)
	{
		float w, h;
		GetWindowSizeF(&w, &h);
		float dx = (f->x - f->anchorX) * w;
		float dy = (f->y - f->anchorY) * h;
		float dist = sqrtf(dx*dx + dy*dy);
		float norm = dist / (STICK_RADIUS * h);

		if (norm >= STICK_DEADZONE)
		{
			if (norm > 1.0f)
				norm = 1.0f;

			float ux = dx / dist;
			float uy = dy / dist;

			gTouchHeld[kNeed_Left]	= ux < -STICK_8WAY_THRESHOLD;
			gTouchHeld[kNeed_Right]	= ux > STICK_8WAY_THRESHOLD;
			gTouchHeld[kNeed_Up]	= uy < -STICK_8WAY_THRESHOLD;
			gTouchHeld[kNeed_Down]	= uy > STICK_8WAY_THRESHOLD;

			gTouchStickMagnitude = (int32_t) (0x10000 * norm);
		}
	}
}

bool TouchControls_GetNeed(int needID)
{
	return gTouchHeld[needID] || gTouchFramePulse[needID];
}

int32_t TouchControls_GetStickMagnitude_Fix32(void)
{
	return gTouchStickMagnitude;
}

int TouchControls_GetOverlayRects(SDL_FRect* outRects, int maxRects)
{
	int n = 0;

	if (!IsGameplayMode())
		return 0;

	for (int i = 0; i < NUM_ZONES && n < maxRects; i++)
		outRects[n++] = GetZoneRect(i);

	TouchFinger* f = GetStickFinger();
	if (f && n + 2 <= maxRects)
	{
		float w, h;
		GetWindowSizeF(&w, &h);
		float rx = STICK_RADIUS * h / w;			// radius in normalized x units
		float ry = STICK_RADIUS;

		outRects[n++] = (SDL_FRect) { f->anchorX - rx, f->anchorY - ry, 2*rx, 2*ry };			// base
		outRects[n++] = (SDL_FRect) { f->x - rx/3, f->y - ry/3, 2*rx/3, 2*ry/3 };				// knob
	}

	return n;
}

void TouchControls_SetBackButtonVisible(bool visible)
{
	gBackButtonWanted = visible;
}

bool TouchControls_GetBackButtonRect(SDL_FRect* outRect)
{
	if (!IsBackButtonVisible())
		return false;

	*outRect = GetBackButtonRect();
	return true;
}

#else

/************************* STUBS (NON-IOS) *********************************/

void TouchControls_Init(void) {}
void TouchControls_Reset(void) {}
void TouchControls_HandleEvent(const SDL_Event* event) { (void) event; }
void TouchControls_Update(void) {}
bool TouchControls_GetNeed(int needID) { (void) needID; return false; }
int32_t TouchControls_GetStickMagnitude_Fix32(void) { return 0; }
int TouchControls_GetOverlayRects(SDL_FRect* outRects, int maxRects) { (void) outRects; (void) maxRects; return 0; }
void TouchControls_SetBackButtonVisible(bool visible) { (void) visible; }
bool TouchControls_GetBackButtonRect(SDL_FRect* outRect) { (void) outRect; return false; }

#endif
