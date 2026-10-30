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

#ifndef BACKENDS_XR_OPENXR_PLATFORM_H
#define BACKENDS_XR_OPENXR_PLATFORM_H

#include "backends/xr/openxr-gl.h"

#include "common/array.h"

#ifdef USE_OPENXR

namespace XR {

/**
 * What the OpenXR code needs from the host: the native window system
 * binding of the OpenGL context, and the way input reaches the engine.
 * One implementation is linked per backend (openxr-sdl.cpp for SDL).
 */
namespace Platform {

/** Whether the user asked for the headset to be used. */
bool isRequested();

/** Prepare the OpenXR loader, before an instance is created. */
bool initLoader();

/** Look up the native window and GL context handles. The GL context must be current. */
bool init();

/** Address of a GL function, or nullptr. */
void *getProcAddress(const char *name);

/** The OpenXR extensions needed to bind the GL context. */
void getExtensions(Common::Array<const char *> &extensions);

/** Structure to chain to the instance creation, or nullptr. */
const void *getInstanceNext();

/** Create a session bound to the current GL context. */
bool createSession(XrInstance instance, XrSystemId systemId, XrSession &session);

/** Size of the window in input coordinates. */
void getWindowSize(int &width, int &height);

/** Size of the GL drawable in pixels. */
void getDrawableSize(int &width, int &height);

/** Input coordinates are in window coordinates. */
void pushMouseMotion(int x, int y);
void pushMouseButton(bool down, int x, int y);
void pushEscape(bool down);

} // End of namespace Platform

} // End of namespace XR

#endif // USE_OPENXR

#endif
