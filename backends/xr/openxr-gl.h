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

#ifndef BACKENDS_XR_OPENXR_GL_H
#define BACKENDS_XR_OPENXR_GL_H

#include "common/scummsys.h"

#ifdef USE_OPENXR

/**
 * OpenGL flavor used by the OpenXR code. Headsets provide desktop OpenGL 4.x
 * or OpenGL ES 3.x, so framebuffer objects and blits are always assumed.
 */
#if defined(__ANDROID__) || (defined(USE_GLES_MODE) && USE_GLES_MODE != 0)
#define XR_GL_ES
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#endif

#ifdef XR_GL_ES
#include <GLES3/gl3.h>
#define XR_USE_GRAPHICS_API_OPENGL_ES
#else
#include <GL/gl.h>
#include <GL/glext.h>
#define XR_USE_GRAPHICS_API_OPENGL
#endif

// A XR_USE_PLATFORM_* macro for the window system binding may be defined before including this file
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#if defined(GL_APIENTRY)
#define XR_GL_CALL GL_APIENTRY
#elif defined(APIENTRY)
#define XR_GL_CALL APIENTRY
#else
#define XR_GL_CALL
#endif

namespace XR {

#ifdef XR_GL_ES
typedef XrSwapchainImageOpenGLESKHR SwapchainImage;
const XrStructureType kSwapchainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
#else
typedef XrSwapchainImageOpenGLKHR SwapchainImage;
const XrStructureType kSwapchainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
#endif

/** The GL entry points used by the OpenXR code, loaded at runtime. */
struct GLFuncs {
	void (XR_GL_CALL *genFramebuffers)(GLsizei, GLuint *);
	void (XR_GL_CALL *deleteFramebuffers)(GLsizei, const GLuint *);
	void (XR_GL_CALL *bindFramebuffer)(GLenum, GLuint);
	void (XR_GL_CALL *framebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
	void (XR_GL_CALL *blitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
	void (XR_GL_CALL *genRenderbuffers)(GLsizei, GLuint *);
	void (XR_GL_CALL *bindRenderbuffer)(GLenum, GLuint);
	void (XR_GL_CALL *renderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
	void (XR_GL_CALL *framebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
	void (XR_GL_CALL *deleteRenderbuffers)(GLsizei, const GLuint *);
	void (XR_GL_CALL *deleteTextures)(GLsizei, const GLuint *);
	void (XR_GL_CALL *genTextures)(GLsizei, GLuint *);
	void (XR_GL_CALL *bindTexture)(GLenum, GLuint);
	void (XR_GL_CALL *texImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
	void (XR_GL_CALL *getIntegerv)(GLenum, GLint *);
	GLboolean (XR_GL_CALL *isEnabled)(GLenum);
	void (XR_GL_CALL *enable)(GLenum);
	void (XR_GL_CALL *disable)(GLenum);
	void (XR_GL_CALL *finish)();
	void (XR_GL_CALL *viewport)(GLint, GLint, GLsizei, GLsizei);
	void (XR_GL_CALL *clear)(GLbitfield);
	void (XR_GL_CALL *clearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
	void (XR_GL_CALL *depthMask)(GLboolean);
	void (XR_GL_CALL *colorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
};

} // End of namespace XR

#endif // USE_OPENXR

#endif
