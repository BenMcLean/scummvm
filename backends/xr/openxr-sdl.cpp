/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/scummsys.h"

#if defined(USE_OPENXR) && defined(SDL_BACKEND)

#include "common/textconsole.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <string.h>
#include <stdlib.h>

#if defined(WIN32)
#define XR_USE_PLATFORM_WIN32
#elif defined(SDL_VIDEO_DRIVER_X11) && SDL_VERSION_ATLEAST(2, 0, 0)
#include <X11/Xlib.h>
#include <GL/glx.h>
#define XR_USE_PLATFORM_XLIB
#endif

#include "backends/xr/openxr-platform.h"

#if defined(XR_USE_PLATFORM_WIN32) || defined(XR_USE_PLATFORM_XLIB)

namespace XR {

namespace Platform {

namespace {

SDL_Window *g_window;

#ifdef XR_USE_PLATFORM_XLIB
Display *g_display;
GLXContext g_glxContext;
GLXDrawable g_glxDrawable;
GLXFBConfig g_fbConfig;
int g_visualId;
#endif

template<class T>
T xrStruct(XrStructureType type) {
	T s;
	memset(&s, 0, sizeof(s));
	s.type = type;
	return s;
}

} // End of anonymous namespace

bool isRequested() {
	return getenv("SCUMMVM_XR") != nullptr;
}

bool initLoader() {
	return true;
}

bool init() {
	g_window = SDL_GL_GetCurrentWindow();
	if (!g_window)
		return false;

#ifdef XR_USE_PLATFORM_XLIB
	SDL_SysWMinfo wm;
	SDL_VERSION(&wm.version);
	if (!SDL_GetWindowWMInfo(g_window, &wm) || wm.subsystem != SDL_SYSWM_X11) {
		warning("OpenXR: needs an X11 window (try SDL_VIDEODRIVER=x11)");
		return false;
	}

	typedef GLXContext (*GetCurrentContextFn)();
	typedef GLXDrawable (*GetCurrentDrawableFn)();
	typedef void (*QueryDrawableFn)(Display *, GLXDrawable, int, unsigned int *);
	typedef GLXFBConfig *(*ChooseFBConfigFn)(Display *, int, const int *, int *);
	typedef int (*GetFBConfigAttribFn)(Display *, GLXFBConfig, int, int *);
	GetCurrentContextFn getCurrentContext = (GetCurrentContextFn)getProcAddress("glXGetCurrentContext");
	GetCurrentDrawableFn getCurrentDrawable = (GetCurrentDrawableFn)getProcAddress("glXGetCurrentDrawable");
	QueryDrawableFn queryDrawable = (QueryDrawableFn)getProcAddress("glXQueryDrawable");
	ChooseFBConfigFn chooseFBConfig = (ChooseFBConfigFn)getProcAddress("glXChooseFBConfig");
	GetFBConfigAttribFn getFBConfigAttrib = (GetFBConfigAttribFn)getProcAddress("glXGetFBConfigAttrib");
	if (!getCurrentContext || !getCurrentDrawable || !queryDrawable || !chooseFBConfig || !getFBConfigAttrib) {
		warning("OpenXR: GLX entry points not available");
		return false;
	}

	g_display = wm.info.x11.display;
	g_glxContext = getCurrentContext();
	g_glxDrawable = getCurrentDrawable();
	if (!g_glxContext || !g_glxDrawable) {
		warning("OpenXR: no current GLX context");
		return false;
	}

	unsigned int fbConfigId = 0;
	queryDrawable(g_display, g_glxDrawable, GLX_FBCONFIG_ID, &fbConfigId);
	int attribs[] = { GLX_FBCONFIG_ID, (int)fbConfigId, None };
	int numConfigs = 0;
	GLXFBConfig *configs = chooseFBConfig(g_display, DefaultScreen(g_display), attribs, &numConfigs);
	if (!configs || numConfigs < 1) {
		warning("OpenXR: could not find the GLX framebuffer config");
		return false;
	}
	g_fbConfig = configs[0];
	getFBConfigAttrib(g_display, g_fbConfig, GLX_VISUAL_ID, &g_visualId);
#endif

	return true;
}

void *getProcAddress(const char *name) {
	return SDL_GL_GetProcAddress(name);
}

void getExtensions(Common::Array<const char *> &extensions) {
	extensions.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
}

const void *getInstanceNext() {
	return nullptr;
}

bool createSession(XrInstance instance, XrSystemId systemId, XrSession &session) {
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
	if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getRequirements)))
		return false;
	XrGraphicsRequirementsOpenGLKHR requirements = xrStruct<XrGraphicsRequirementsOpenGLKHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR);
	if (XR_FAILED(getRequirements(instance, systemId, &requirements)))
		return false;

	XrSessionCreateInfo sessionInfo = xrStruct<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
	sessionInfo.systemId = systemId;

#if defined(XR_USE_PLATFORM_XLIB)
	XrGraphicsBindingOpenGLXlibKHR binding = xrStruct<XrGraphicsBindingOpenGLXlibKHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR);
	binding.xDisplay = g_display;
	binding.visualid = g_visualId;
	binding.glxFBConfig = g_fbConfig;
	binding.glxDrawable = g_glxDrawable;
	binding.glxContext = g_glxContext;
#elif defined(XR_USE_PLATFORM_WIN32)
	XrGraphicsBindingOpenGLWin32KHR binding = xrStruct<XrGraphicsBindingOpenGLWin32KHR>(XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR);
	// opengl32 is loaded at runtime by SDL
	HMODULE opengl = GetModuleHandleA("opengl32.dll");
	typedef HDC (WINAPI *GetCurrentDCFn)();
	typedef HGLRC (WINAPI *GetCurrentContextFn)();
	GetCurrentDCFn getCurrentDC = opengl ? (GetCurrentDCFn)(void *)GetProcAddress(opengl, "wglGetCurrentDC") : nullptr;
	GetCurrentContextFn getCurrentContext = opengl ? (GetCurrentContextFn)(void *)GetProcAddress(opengl, "wglGetCurrentContext") : nullptr;
	if (!getCurrentDC || !getCurrentContext)
		return false;
	binding.hDC = getCurrentDC();
	binding.hGLRC = getCurrentContext();
#endif
	sessionInfo.next = &binding;
	return XR_SUCCEEDED(xrCreateSession(instance, &sessionInfo, &session));
}

void getWindowSize(int &width, int &height) {
	SDL_GetWindowSize(g_window, &width, &height);
}

void getDrawableSize(int &width, int &height) {
	SDL_GL_GetDrawableSize(g_window, &width, &height);
}

void pushMouseMotion(int x, int y) {
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_MOUSEMOTION;
	ev.motion.windowID = SDL_GetWindowID(g_window);
	ev.motion.x = x;
	ev.motion.y = y;
	SDL_PushEvent(&ev);
}

void pushMouseButton(bool down, int x, int y) {
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	ev.button.windowID = SDL_GetWindowID(g_window);
	ev.button.button = SDL_BUTTON_LEFT;
	ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.button.clicks = 1;
	ev.button.x = x;
	ev.button.y = y;
	SDL_PushEvent(&ev);
}

void pushEscape(bool down) {
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.windowID = SDL_GetWindowID(g_window);
	ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
	ev.key.keysym.sym = SDLK_ESCAPE;
	SDL_PushEvent(&ev);
}

} // End of namespace Platform

} // End of namespace XR

#endif // XR_USE_PLATFORM_WIN32 || XR_USE_PLATFORM_XLIB

#endif // USE_OPENXR && SDL_BACKEND
