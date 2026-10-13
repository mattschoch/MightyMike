#if !(GLRENDER)

#include <SDL3/SDL.h>
#include "myglobals.h"
#include "externs.h"
#include "misc.h"
#include "renderdrivers.h"
#include "framebufferfilter.h"
#include "touchcontrols.h"

#if _DEBUG
#define CHECK_SDL_ERROR(success)										\
	do {					 											\
		if (!success)													\
			DoFatalSDLError(success, __func__, __LINE__);				\
	} while(0)

static void DoFatalSDLError(int error, const char* file, int line)
{
	static char alertbuf[1024];
	SDL_snprintf(alertbuf, sizeof(alertbuf), "SDL error %d\nin %s:%d\n%s", error, file, line, SDL_GetError());
	DoFatalAlert(alertbuf);
}
#else
#define CHECK_SDL_ERROR(success) (void) (success)
#endif

static SDL_Renderer*	gSDLRenderer		= NULL;
static SDL_Texture*		gSDLTexture			= NULL;
static color_t*			gFinalFramebuffer	= NULL;
const char*				gRendererName		= "NULL";
Boolean					gCanDoHQStretch		= true;

Boolean SDLRender_Init(void)
{
	gSDLRenderer = SDL_CreateRenderer(gSDLWindow, NULL);
	if (!gSDLRenderer)
		return false;
	// The texture bound to the renderer is created in-game after loading the prefs.

#if !(NOVSYNC)
	SDL_SetRenderVSync(gSDLRenderer, 1);
#endif
	
	const char* sdlRendererName = SDL_GetRendererName(gSDLRenderer);
	if (sdlRendererName)
	{
		static char rendererName[32];
		SDL_snprintf(rendererName, sizeof(rendererName), "sdl-%s-%d", sdlRendererName, (int) sizeof(color_t) * 8);
		gRendererName = rendererName;
	}
	
	SDL_SetRenderLogicalPresentation(gSDLRenderer, VISIBLE_WIDTH, VISIBLE_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);

	return true;
}

static void SDLRender_NukeTextureAndBuffers(void)
{
	if (gFinalFramebuffer)
	{
		DisposePtr((Ptr) gFinalFramebuffer);
		gFinalFramebuffer = NULL;
	}

	if (gSDLTexture)
	{
		SDL_DestroyTexture(gSDLTexture);
		gSDLTexture = NULL;
	}
}

void SDLRender_Shutdown(void)
{
	ShutdownRenderThreads();

	SDLRender_NukeTextureAndBuffers();

	if (gSDLRenderer)
	{
		SDL_DestroyRenderer(gSDLRenderer);
		gSDLRenderer = NULL;
	}
}

void SDLRender_InitTexture(void)
{
	// Nuke old texture and RGBA buffers
	SDLRender_NukeTextureAndBuffers();

	bool crisp = (gEffectiveScalingType == kScaling_PixelPerfect);
	int textureSizeMultiplier = (gEffectiveScalingType == kScaling_HQStretch) ? 2 : 1;

	// Allocate buffer
	gFinalFramebuffer = (color_t*) NewPtrClear((VISIBLE_WIDTH * 2) * (VISIBLE_HEIGHT * 2) * (int) sizeof(color_t));
	GAME_ASSERT(gFinalFramebuffer);

	// Recreate texture
	gSDLTexture = SDL_CreateTexture(
			gSDLRenderer,
#if FRAMEBUFFER_COLOR_DEPTH == 16
			SDL_PIXELFORMAT_RGB565,
#else
			SDL_PIXELFORMAT_RGBA8888,
#endif
			SDL_TEXTUREACCESS_STREAMING,
			VISIBLE_WIDTH * textureSizeMultiplier,
			VISIBLE_HEIGHT * textureSizeMultiplier);
	GAME_ASSERT(gSDLTexture);

	// Set scaling quality
	SDL_SetTextureScaleMode(gSDLTexture, crisp ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);

	// Set logical size
	SDL_SetRenderLogicalPresentation(gSDLRenderer, VISIBLE_WIDTH, VISIBLE_HEIGHT, crisp ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE : SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

#ifdef SDL_PLATFORM_IOS
/************************* TOUCH OVERLAY *********************************/
//
// Drawn in output pixels, with logical presentation disabled.
//

static void SDLRender_FillQuad(const SDL_FPoint p[4], SDL_FColor color)
{
	SDL_Vertex v[4];
	for (int i = 0; i < 4; i++)
		v[i] = (SDL_Vertex) { .position = p[i], .color = color };
	static const int kIndices[6] = { 0, 1, 2, 0, 2, 3 };
	SDL_RenderGeometry(gSDLRenderer, NULL, v, 4, kIndices, 6);
}

static void SDLRender_ThickLine(float x1, float y1, float x2, float y2, float thick, SDL_FColor color)
{
	float dx = x2 - x1;
	float dy = y2 - y1;
	float len = SDL_sqrtf(dx*dx + dy*dy);
	if (len <= 0.0f)
		return;

	float nx = -dy / len * thick * 0.5f;
	float ny = dx / len * thick * 0.5f;
	SDL_FPoint p[4] = { {x1+nx, y1+ny}, {x2+nx, y2+ny}, {x2-nx, y2-ny}, {x1-nx, y1-ny} };
	SDLRender_FillQuad(p, color);
}

static void SDLRender_Circle(float cx, float cy, float r, float thick, bool filled, SDL_FColor color)
{
	enum { kSegments = 32 };
	SDL_Vertex v[kSegments + 1];
	int indices[kSegments * 3];

	if (!filled)
	{
		for (int i = 0; i < kSegments; i++)
		{
			float a0 = (float) i * 2.0f * SDL_PI_F / kSegments;
			float a1 = (float) (i + 1) * 2.0f * SDL_PI_F / kSegments;
			SDLRender_ThickLine(cx + r*SDL_cosf(a0), cy + r*SDL_sinf(a0), cx + r*SDL_cosf(a1), cy + r*SDL_sinf(a1), thick, color);
		}
		return;
	}

	v[kSegments] = (SDL_Vertex) { .position = {cx, cy}, .color = color };
	for (int i = 0; i < kSegments; i++)
	{
		float a = (float) i * 2.0f * SDL_PI_F / kSegments;
		v[i] = (SDL_Vertex) { .position = {cx + r*SDL_cosf(a), cy + r*SDL_sinf(a)}, .color = color };
		indices[i*3+0] = kSegments;
		indices[i*3+1] = i;
		indices[i*3+2] = (i + 1) % kSegments;
	}
	SDL_RenderGeometry(gSDLRenderer, NULL, v, kSegments + 1, indices, kSegments * 3);
}

static void SDLRender_DrawTouchButton(int which, const SDL_FRect* r, float thick)
{
	const SDL_FColor kFill		= { 1.0f, 1.0f, 1.0f, 0.18f };
	const SDL_FColor kOutline	= { 1.0f, 1.0f, 1.0f, 0.55f };
	const SDL_FColor kGlyph		= { 1.0f, 1.0f, 1.0f, 0.80f };

	// Visual inset inside the touch zone, so neighbors are separated
	// and the screen's rounded corner doesn't clip the rightmost button
	float inset = thick * 2.0f;
	SDL_FRect b = { r->x + inset, r->y + inset, r->w - 2*inset, r->h - 2*inset };
	if (which == kTouchOverlay_Pause)
		b.w -= inset * 2.0f;
	else if (which == kTouchOverlay_Back)				// mirror of Pause, for the top-left corner
	{
		b.x += inset * 2.0f;
		b.w -= inset * 2.0f;
	}

	SDL_SetRenderDrawColorFloat(gSDLRenderer, kFill.r, kFill.g, kFill.b, kFill.a);
	SDL_RenderFillRect(gSDLRenderer, &b);

	SDL_SetRenderDrawColorFloat(gSDLRenderer, kOutline.r, kOutline.g, kOutline.b, kOutline.a);
	SDL_FRect edges[4] =
	{
		{ b.x,				b.y,				b.w,	thick },
		{ b.x,				b.y + b.h - thick,	b.w,	thick },
		{ b.x,				b.y + thick,		thick,	b.h - 2*thick },
		{ b.x + b.w - thick,	b.y + thick,		thick,	b.h - 2*thick },
	};
	SDL_RenderFillRects(gSDLRenderer, edges, 4);

	float cx = b.x + b.w * 0.5f;
	float cy = b.y + b.h * 0.5f;
	float s = (b.w < b.h ? b.w : b.h) * 0.30f;			// glyph half-size

	switch (which)
	{
		case kTouchOverlay_NextWeapon:					// ">>"
			for (int i = 0; i < 2; i++)
			{
				float x = cx - s*0.75f + i * s*0.9f;
				SDLRender_ThickLine(x, cy - s, x + s*0.6f, cy, thick*1.5f, kGlyph);
				SDLRender_ThickLine(x + s*0.6f, cy, x, cy + s, thick*1.5f, kGlyph);
			}
			break;

		case kTouchOverlay_Radar:						// ring + crosshair + center dot
			SDLRender_Circle(cx, cy, s, thick*1.5f, false, kGlyph);
			SDLRender_ThickLine(cx - s*1.3f, cy, cx + s*1.3f, cy, thick, kGlyph);
			SDLRender_ThickLine(cx, cy - s*1.3f, cx, cy + s*1.3f, thick, kGlyph);
			SDLRender_Circle(cx, cy, thick*1.5f, 0, true, kGlyph);
			break;

		case kTouchOverlay_Pause:						// "||"
		{
			SDL_SetRenderDrawColorFloat(gSDLRenderer, kGlyph.r, kGlyph.g, kGlyph.b, kGlyph.a);
			SDL_FRect bars[2] =
			{
				{ cx - s*0.65f, cy - s, s*0.45f, 2*s },
				{ cx + s*0.20f, cy - s, s*0.45f, 2*s },
			};
			SDL_RenderFillRects(gSDLRenderer, bars, 2);
			break;
		}

		case kTouchOverlay_Back:						// "<"
			SDLRender_ThickLine(cx + s*0.35f, cy - s, cx - s*0.45f, cy, thick*1.5f, kGlyph);
			SDLRender_ThickLine(cx - s*0.45f, cy, cx + s*0.35f, cy + s, thick*1.5f, kGlyph);
			break;
	}
}

static void SDLRender_DrawTouchOverlay(void)
{
	SDL_FRect rects[kTouchOverlay_COUNT];
	SDL_zeroa(rects);
	int numRects = TouchControls_GetOverlayRects(rects, kTouchOverlay_Back);
	bool hasBack = TouchControls_GetBackButtonRect(&rects[kTouchOverlay_Back]);
	if (numRects <= 0 && !hasBack)
		return;

	int outW = 0;
	int outH = 0;
	SDL_GetCurrentRenderOutputSize(gSDLRenderer, &outW, &outH);

	for (int i = 0; i < kTouchOverlay_COUNT; i++)
	{
		rects[i].x *= outW;
		rects[i].y *= outH;
		rects[i].w *= outW;
		rects[i].h *= outH;
	}

	float thick = SDL_max(2.0f, SDL_roundf(outH / 240.0f));

	SDL_SetRenderDrawBlendMode(gSDLRenderer, SDL_BLENDMODE_BLEND);

	for (int i = 0; i < numRects && i < kTouchOverlay_StickBase; i++)
		SDLRender_DrawTouchButton(i, &rects[i], thick);

	if (numRects > kTouchOverlay_StickKnob)
	{
		const SDL_FRect* base = &rects[kTouchOverlay_StickBase];
		const SDL_FRect* knob = &rects[kTouchOverlay_StickKnob];

		SDLRender_Circle(base->x + base->w*0.5f, base->y + base->h*0.5f, base->w*0.5f, 0, true, (SDL_FColor) {1, 1, 1, 0.12f});
		SDLRender_Circle(base->x + base->w*0.5f, base->y + base->h*0.5f, base->w*0.5f, thick, false, (SDL_FColor) {1, 1, 1, 0.55f});
		SDLRender_Circle(knob->x + knob->w*0.5f, knob->y + knob->h*0.5f, knob->w*0.5f, 0, true, (SDL_FColor) {1, 1, 1, 0.60f});
	}

	if (hasBack)
		SDLRender_DrawTouchButton(kTouchOverlay_Back, &rects[kTouchOverlay_Back], thick);

	SDL_SetRenderDrawColor(gSDLRenderer, 0, 0, 0, 255);		// restore clear color
}
#endif

void SDLRender_PresentFramebuffer(void)
{
	bool success = true;

	//-------------------------------------------------------------------------
	// Convert indexed to RGBA, with optional post-processing

	ConvertFramebufferMT(gFinalFramebuffer);

	//-------------------------------------------------------------------------
	// Update SDL texture

	int pitch = VISIBLE_WIDTH * (int) sizeof(color_t);

	if (gEffectiveScalingType == kScaling_HQStretch)
		pitch *= 2;

	success = SDL_UpdateTexture(gSDLTexture, NULL, gFinalFramebuffer, pitch);
	CHECK_SDL_ERROR(success);

	//-------------------------------------------------------------------------
	// Present it

	SDL_RenderClear(gSDLRenderer);
	success = SDL_RenderTexture(gSDLRenderer, gSDLTexture, NULL, NULL);
	CHECK_SDL_ERROR(success);
#ifdef SDL_PLATFORM_IOS
	// Draw the touch overlay over the whole window, letterbox bars included.
	// Logical presentation must stay off until after SDL_RenderPresent, which would
	// otherwise paint over the bars. SDL doesn't reset the viewport by itself mid-frame.
	int logicalW = 0;
	int logicalH = 0;
	SDL_RendererLogicalPresentation logicalMode = SDL_LOGICAL_PRESENTATION_DISABLED;
	SDL_GetRenderLogicalPresentation(gSDLRenderer, &logicalW, &logicalH, &logicalMode);
	SDL_SetRenderLogicalPresentation(gSDLRenderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
	SDL_SetRenderViewport(gSDLRenderer, NULL);

	SDLRender_DrawTouchOverlay();
	SDL_RenderPresent(gSDLRenderer);

	SDL_SetRenderLogicalPresentation(gSDLRenderer, logicalW, logicalH, logicalMode);
	SDL_SetRenderViewport(gSDLRenderer, NULL);
#else
	SDL_RenderPresent(gSDLRenderer);
#endif
}

#endif
