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

#ifdef USE_OPENXR

#include "common/array.h"
#include "common/debug.h"
#include "common/str.h"
#include "common/system.h"
#include "common/textconsole.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "backends/xr/openxr-platform.h"
#include "backends/xr/openxr-theatre.h"

namespace XR {

namespace {

enum State {
	kUninitialized,
	kRunning,
	kFailed
};

enum { kHandLeft, kHandRight, kHandCount };
enum { kEyeCount = 2 };

const float kQuadDistance = 2.0f;
const float kQuadWidth = 3.2f;

// Thumbstick turning
const float kSmoothTurnSpeed = 120.0f; // degrees per second at full deflection
const float kSnapTurnAngle = 45.0f;
const float kTurnDeadZone = 0.15f;
const float kSnapThreshold = 0.7f;
const float kSnapRelease = 0.3f;

// A stereo frame submitted within this time means the engine is driving the headset
const uint32 kStereoTimeoutMs = 150;

struct Context {
	State state;
	GLFuncs gl;

	XrInstance instance;
	XrSystemId systemId;
	XrSession session;
	XrSpace space;
	XrSpace viewSpace;
	bool sessionRunning;
	int64_t chainFormat;

	// Window contents, copied right before each buffer swap
	GLuint mirrorTex, mirrorFBO;
	int mirrorW, mirrorH;
	bool mirrorValid;

	// Swapchain carrying the mirror to the compositor (theatre mode)
	XrSwapchain swapchain;
	SwapchainImage *images;
	uint32 imageCount;
	GLuint chainFBO;
	int chainW, chainH;

	// Per eye swapchains (stereo mode)
	XrSwapchain eyeChain[kEyeCount];
	SwapchainImage *eyeImages[kEyeCount];
	uint32 eyeImageCount[kEyeCount];
	int eyeW, eyeH;
	GLuint eyeFBO, eyeDepth;
	GLint savedFBO;
	GLint savedViewport[4];
	bool eyeBound;

	// Frame state
	bool frameOpen;
	XrFrameState frameState;
	XrView views[kEyeCount];
	EyeView eyeViews[kEyeCount];
	bool viewsValid;
	bool stereoRendered;
	uint32 stereoTicks;
	bool stereoEverSubmitted;

	float headYaw, headPitch;
	bool headValid;
	XrPosef headPose;

	// The theatre quad
	XrPosef quadPose;
	bool quadAnchored;
	float aimYaw, aimPitch;
	float aimDir[3];
	bool aimValid;

	XrActionSet actionSet;
	XrAction aimAction, selectAction, menuAction, turnAction, turnClickAction;
	XrPath handPath[kHandCount];
	XrSpace aimSpace[kHandCount];
	bool selectDown[kHandCount];
	bool menuDown[kHandCount];
	int activeHand;
	bool buttonHeld;
	int lastMouseX, lastMouseY;

	// Turning with the thumbstick
	bool snapTurn;
	bool snapLatched;
	float pendingTurn;
	uint32 lastTurnTicks;

	uint32 lastFrameTicks;
	uint32 periodMs;

	Context() {
		memset(this, 0, sizeof(*this));
		state = kUninitialized;
		instance = XR_NULL_HANDLE;
		session = XR_NULL_HANDLE;
		space = XR_NULL_HANDLE;
		viewSpace = XR_NULL_HANDLE;
		swapchain = XR_NULL_HANDLE;
		for (int e = 0; e < kEyeCount; e++)
			eyeChain[e] = XR_NULL_HANDLE;
		actionSet = XR_NULL_HANDLE;
		aimAction = selectAction = menuAction = turnAction = turnClickAction = XR_NULL_HANDLE;
		for (int h = 0; h < kHandCount; h++) {
			handPath[h] = XR_NULL_PATH;
			aimSpace[h] = XR_NULL_HANDLE;
		}
		activeHand = kHandRight;
		periodMs = 11;
		headPose.orientation.w = 1.0f;
		quadPose.orientation.w = 1.0f;
		quadPose.position.z = -kQuadDistance;
	}
};

Context *g_ctx = nullptr;

template<class T>
T xrStruct(XrStructureType type) {
	T s;
	memset(&s, 0, sizeof(s));
	s.type = type;
	return s;
}

bool check(XrResult result, const char *what) {
	if (XR_FAILED(result)) {
		warning("OpenXR: %s failed (%d)", what, (int)result);
		return false;
	}
	return true;
}

#define XR_TRY(x) do { if (!check((x), #x)) return false; } while (0)

template<class T>
bool loadGL(T &fn, const char *name) {
	fn = (T)Platform::getProcAddress(name);
	return fn != nullptr;
}

bool loadGLFuncs(GLFuncs &gl) {
	return loadGL(gl.genFramebuffers, "glGenFramebuffers") &&
	       loadGL(gl.deleteFramebuffers, "glDeleteFramebuffers") &&
	       loadGL(gl.bindFramebuffer, "glBindFramebuffer") &&
	       loadGL(gl.framebufferTexture2D, "glFramebufferTexture2D") &&
	       loadGL(gl.blitFramebuffer, "glBlitFramebuffer") &&
	       loadGL(gl.genRenderbuffers, "glGenRenderbuffers") &&
	       loadGL(gl.bindRenderbuffer, "glBindRenderbuffer") &&
	       loadGL(gl.renderbufferStorage, "glRenderbufferStorage") &&
	       loadGL(gl.framebufferRenderbuffer, "glFramebufferRenderbuffer") &&
	       loadGL(gl.deleteRenderbuffers, "glDeleteRenderbuffers") &&
	       loadGL(gl.deleteTextures, "glDeleteTextures") &&
	       loadGL(gl.genTextures, "glGenTextures") &&
	       loadGL(gl.bindTexture, "glBindTexture") &&
	       loadGL(gl.texImage2D, "glTexImage2D") &&
	       loadGL(gl.getIntegerv, "glGetIntegerv") &&
	       loadGL(gl.isEnabled, "glIsEnabled") &&
	       loadGL(gl.enable, "glEnable") &&
	       loadGL(gl.disable, "glDisable") &&
	       loadGL(gl.finish, "glFinish") &&
	       loadGL(gl.viewport, "glViewport") &&
	       loadGL(gl.clear, "glClear") &&
	       loadGL(gl.clearColor, "glClearColor") &&
	       loadGL(gl.depthMask, "glDepthMask") &&
	       loadGL(gl.colorMask, "glColorMask");
}

/** Saves the GL state touched while copying, and restores it on scope exit. */
class GLStateSaver {
public:
	GLStateSaver(GLFuncs &gl) : _gl(gl) {
		_gl.getIntegerv(GL_READ_FRAMEBUFFER_BINDING, &_read);
		_gl.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &_draw);
		_gl.getIntegerv(GL_TEXTURE_BINDING_2D, &_tex);
#ifdef GL_FRAMEBUFFER_SRGB
		_srgb = _gl.isEnabled(GL_FRAMEBUFFER_SRGB);
		_gl.disable(GL_FRAMEBUFFER_SRGB);
#endif
	}
	~GLStateSaver() {
#ifdef GL_FRAMEBUFFER_SRGB
		if (_srgb)
			_gl.enable(GL_FRAMEBUFFER_SRGB);
#endif
		_gl.bindTexture(GL_TEXTURE_2D, _tex);
		_gl.bindFramebuffer(GL_READ_FRAMEBUFFER, _read);
		_gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, _draw);
	}
private:
	GLFuncs &_gl;
	GLint _read, _draw, _tex;
	GLboolean _srgb = GL_FALSE;
};

float radToDeg(float r) {
	return r * (180.0f / (float)M_PI);
}

/** Decompose an orientation into yaw (around Y), pitch (around X) and roll (around Z), applied in that order. */
void orientationToEuler(const XrQuaternionf &q, float &yaw, float &pitch, float &roll) {
	const float m02 = 2.0f * (q.x * q.z + q.w * q.y);
	const float m12 = 2.0f * (q.y * q.z - q.w * q.x);
	const float m22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
	const float m10 = 2.0f * (q.x * q.y + q.w * q.z);
	const float m11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);

	yaw = radToDeg(atan2f(m02, m22));
	pitch = radToDeg(asinf(CLIP(-m12, -1.0f, 1.0f)));
	roll = radToDeg(atan2f(m10, m11));
}

float dot3(const float *a, const float *b) {
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/** Rotate a vector by a unit quaternion, in place. */
void rotateVector(const XrQuaternionf &q, float *v) {
	const float t[3] = {
		2.0f * (q.y * v[2] - q.z * v[1]),
		2.0f * (q.z * v[0] - q.x * v[2]),
		2.0f * (q.x * v[1] - q.y * v[0])
	};
	const float r[3] = {
		v[0] + q.w * t[0] + (q.y * t[2] - q.z * t[1]),
		v[1] + q.w * t[1] + (q.z * t[0] - q.x * t[2]),
		v[2] + q.w * t[2] + (q.x * t[1] - q.y * t[0])
	};
	v[0] = r[0];
	v[1] = r[1];
	v[2] = r[2];
}

XrQuaternionf multiplyQuaternions(const XrQuaternionf &a, const XrQuaternionf &b) {
	XrQuaternionf r;
	r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
	return r;
}

/** The forward vector (0, 0, -1) rotated by q. */
void forwardVector(const XrQuaternionf &q, float &x, float &y, float &z) {
	x = -2.0f * (q.x * q.z + q.w * q.y);
	y = -2.0f * (q.y * q.z - q.w * q.x);
	z = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
}

enum { kBothHands, kLeftOnly, kRightOnly };

struct BindingDef {
	int action;
	const char *suffix;
	int hands;
};

enum { kActionAim, kActionSelect, kActionMenu, kActionTurn, kActionTurnClick, kActionCount };

bool suggestBindings(Context &c, const XrAction *actions, const char *profileName, const BindingDef *defs, uint count, bool required) {
	XrPath profile;
	XR_TRY(xrStringToPath(c.instance, profileName, &profile));

	static const char *const kHands[kHandCount] = { "/user/hand/left", "/user/hand/right" };
	Common::Array<XrActionSuggestedBinding> bindings;
	for (uint i = 0; i < count; i++) {
		for (int h = 0; h < kHandCount; h++) {
			if ((defs[i].hands == kLeftOnly && h != kHandLeft) || (defs[i].hands == kRightOnly && h != kHandRight))
				continue;
			XrActionSuggestedBinding binding;
			binding.action = actions[defs[i].action];
			const Common::String path = Common::String(kHands[h]) + defs[i].suffix;
			XR_TRY(xrStringToPath(c.instance, path.c_str(), &binding.binding));
			bindings.push_back(binding);
		}
	}

	XrInteractionProfileSuggestedBinding suggest = xrStruct<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
	suggest.interactionProfile = profile;
	suggest.countSuggestedBindings = bindings.size();
	suggest.suggestedBindings = bindings.data();
	const XrResult result = xrSuggestInteractionProfileBindings(c.instance, &suggest);
	if (XR_FAILED(result)) {
		if (required)
			return check(result, profileName);
		debug(1, "OpenXR: bindings for %s not accepted (%d)", profileName, (int)result);
	}
	return true;
}

bool createActions(Context &c) {
	XR_TRY(xrStringToPath(c.instance, "/user/hand/left", &c.handPath[kHandLeft]));
	XR_TRY(xrStringToPath(c.instance, "/user/hand/right", &c.handPath[kHandRight]));

	XrActionSetCreateInfo setInfo = xrStruct<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
	strcpy(setInfo.actionSetName, "gameplay");
	strcpy(setInfo.localizedActionSetName, "Gameplay");
	XR_TRY(xrCreateActionSet(c.instance, &setInfo, &c.actionSet));

	struct { XrActionType type; const char *name; } defs[kActionCount] = {
		{ XR_ACTION_TYPE_POSE_INPUT, "aim" },
		{ XR_ACTION_TYPE_BOOLEAN_INPUT, "select" },
		{ XR_ACTION_TYPE_BOOLEAN_INPUT, "menu" },
		{ XR_ACTION_TYPE_FLOAT_INPUT, "turn" },
		{ XR_ACTION_TYPE_BOOLEAN_INPUT, "turn_click" }
	};
	XrAction actions[kActionCount];
	for (int i = 0; i < kActionCount; i++) {
		XrActionCreateInfo info = xrStruct<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
		info.actionType = defs[i].type;
		strcpy(info.actionName, defs[i].name);
		strcpy(info.localizedActionName, defs[i].name);
		info.countSubactionPaths = kHandCount;
		info.subactionPaths = c.handPath;
		XR_TRY(xrCreateAction(c.actionSet, &info, &actions[i]));
	}
	c.aimAction = actions[kActionAim];
	c.selectAction = actions[kActionSelect];
	c.menuAction = actions[kActionMenu];
	c.turnAction = actions[kActionTurn];
	c.turnClickAction = actions[kActionTurnClick];

	// Standard Khronos interaction profiles; the runtime maps the connected device
	static const BindingDef simple[] = {
		{ kActionAim, "/input/aim/pose", kBothHands },
		{ kActionSelect, "/input/select/click", kBothHands },
		{ kActionMenu, "/input/menu/click", kBothHands }
	};
	static const BindingDef thumbstickMenuLeft[] = {
		{ kActionAim, "/input/aim/pose", kBothHands },
		{ kActionSelect, "/input/trigger/value", kBothHands },
		{ kActionMenu, "/input/menu/click", kLeftOnly },
		{ kActionMenu, "/input/b/click", kRightOnly },
		{ kActionTurn, "/input/thumbstick/x", kBothHands },
		{ kActionTurnClick, "/input/thumbstick/click", kBothHands }
	};
	static const BindingDef thumbstickMenuB[] = {
		{ kActionAim, "/input/aim/pose", kBothHands },
		{ kActionSelect, "/input/trigger/value", kBothHands },
		{ kActionMenu, "/input/b/click", kBothHands },
		{ kActionTurn, "/input/thumbstick/x", kBothHands },
		{ kActionTurnClick, "/input/thumbstick/click", kBothHands }
	};

	return suggestBindings(c, actions, "/interaction_profiles/khr/simple_controller", simple, ARRAYSIZE(simple), true) &&
	       suggestBindings(c, actions, "/interaction_profiles/oculus/touch_controller", thumbstickMenuLeft, ARRAYSIZE(thumbstickMenuLeft), false) &&
	       suggestBindings(c, actions, "/interaction_profiles/valve/index_controller", thumbstickMenuB, ARRAYSIZE(thumbstickMenuB), false);
}

bool createSession(Context &c) {
	Common::Array<const char *> extensions;
	Platform::getExtensions(extensions);
	XrInstanceCreateInfo instanceInfo = xrStruct<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
	strcpy(instanceInfo.applicationInfo.applicationName, "ScummVM");
	instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instanceInfo.next = Platform::getInstanceNext();
	instanceInfo.enabledExtensionCount = extensions.size();
	instanceInfo.enabledExtensionNames = extensions.data();
	XR_TRY(xrCreateInstance(&instanceInfo, &c.instance));

	XrSystemGetInfo systemInfo = xrStruct<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_TRY(xrGetSystem(c.instance, &systemInfo, &c.systemId));

	if (!Platform::createSession(c.instance, c.systemId, c.session)) {
		warning("OpenXR: could not create a session for the GL context");
		return false;
	}

	XrReferenceSpaceCreateInfo spaceInfo = xrStruct<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
	XR_TRY(xrCreateReferenceSpace(c.session, &spaceInfo, &c.space));
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	XR_TRY(xrCreateReferenceSpace(c.session, &spaceInfo, &c.viewSpace));

	XrViewConfigurationView viewConfigs[kEyeCount];
	for (int e = 0; e < kEyeCount; e++)
		viewConfigs[e] = xrStruct<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW);
	uint32_t viewCount = 0;
	XR_TRY(xrEnumerateViewConfigurationViews(c.instance, c.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, kEyeCount, &viewCount, viewConfigs));
	c.eyeW = viewConfigs[0].recommendedImageRectWidth;
	c.eyeH = viewConfigs[0].recommendedImageRectHeight;

	uint32_t formatCount = 0;
	XR_TRY(xrEnumerateSwapchainFormats(c.session, 0, &formatCount, nullptr));
	int64_t *formats = new int64_t[formatCount];
	xrEnumerateSwapchainFormats(c.session, formatCount, &formatCount, formats);
	c.chainFormat = 0;
	for (uint32_t i = 0; i < formatCount; i++) {
		if (formats[i] == GL_SRGB8_ALPHA8)
			c.chainFormat = formats[i];
	}
	for (uint32_t i = 0; i < formatCount && !c.chainFormat; i++) {
		if (formats[i] == GL_RGBA8)
			c.chainFormat = formats[i];
	}
	delete[] formats;
	if (!c.chainFormat) {
		warning("OpenXR: no usable swapchain format");
		return false;
	}

	if (!createActions(c))
		return false;

	XrSessionActionSetsAttachInfo attachInfo = xrStruct<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &c.actionSet;
	XR_TRY(xrAttachSessionActionSets(c.session, &attachInfo));

	for (int h = 0; h < kHandCount; h++) {
		XrActionSpaceCreateInfo info = xrStruct<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
		info.action = c.aimAction;
		info.subactionPath = c.handPath[h];
		info.poseInActionSpace.orientation.w = 1.0f;
		XR_TRY(xrCreateActionSpace(c.session, &info, &c.aimSpace[h]));
	}

	return true;
}

void destroySwapchain(XrSwapchain &chain, SwapchainImage *&images, uint32 &count) {
	if (chain != XR_NULL_HANDLE) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}
	delete[] images;
	images = nullptr;
	count = 0;
}

bool createSwapchain(Context &c, int w, int h, XrSwapchain &chain, SwapchainImage *&images, uint32 &count) {
	destroySwapchain(chain, images, count);

	XrSwapchainCreateInfo info = xrStruct<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	info.format = c.chainFormat;
	info.sampleCount = 1;
	info.width = w;
	info.height = h;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	XR_TRY(xrCreateSwapchain(c.session, &info, &chain));

	XR_TRY(xrEnumerateSwapchainImages(chain, 0, &count, nullptr));
	images = new SwapchainImage[count];
	for (uint32 i = 0; i < count; i++)
		images[i] = xrStruct<SwapchainImage>(kSwapchainImageType);
	XR_TRY(xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader *)images));
	return true;
}

void fail(Context &c) {
	c.state = kFailed;
	c.sessionRunning = false;
}

bool init(Context &c) {
	if (!Platform::initLoader() || !Platform::init() || !loadGLFuncs(c.gl)) {
		warning("OpenXR: could not get the window or required GL functions");
		return false;
	}
	if (!createSession(c))
		return false;
	c.gl.genFramebuffers(1, &c.mirrorFBO);
	c.gl.genFramebuffers(1, &c.chainFBO);
	c.gl.genFramebuffers(1, &c.eyeFBO);
	c.gl.genTextures(1, &c.mirrorTex);
	return true;
}

Context *getContext() {
	if (g_ctx)
		return g_ctx->state == kRunning ? g_ctx : nullptr;
	if (!Platform::isRequested())
		return nullptr;

	g_ctx = new Context();
	if (init(*g_ctx)) {
		g_ctx->state = kRunning;
		debug("OpenXR: initialised");
	} else {
		fail(*g_ctx);
	}
	return g_ctx->state == kRunning ? g_ctx : nullptr;
}

void handleEvents(Context &c) {
	XrEventDataBuffer event = xrStruct<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
	while (xrPollEvent(c.instance, &event) == XR_SUCCESS) {
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const XrEventDataSessionStateChanged *changed = (const XrEventDataSessionStateChanged *)&event;
			debug(1, "OpenXR: session state %d", (int)changed->state);
			switch (changed->state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo beginInfo = xrStruct<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (check(xrBeginSession(c.session, &beginInfo), "xrBeginSession"))
					c.sessionRunning = true;
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				xrEndSession(c.session);
				c.sessionRunning = false;
				c.frameOpen = false;
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				fail(c);
				return;
			default:
				break;
			}
		}
		if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			for (int h = 0; h < kHandCount; h++) {
				XrInteractionProfileState profile = xrStruct<XrInteractionProfileState>(XR_TYPE_INTERACTION_PROFILE_STATE);
				if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(c.session, c.handPath[h], &profile)) && profile.interactionProfile != XR_NULL_PATH) {
					char name[XR_MAX_PATH_LENGTH];
					uint32_t length = 0;
					if (XR_SUCCEEDED(xrPathToString(c.instance, profile.interactionProfile, sizeof(name), &length, name)))
						debug(1, "OpenXR: hand %d uses the interaction profile %s", h, name);
				}
			}
		}
		event = xrStruct<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
	}
}

bool stereoRecent(const Context &c) {
	return c.stereoEverSubmitted && g_system->getMillis(true) - c.stereoTicks < kStereoTimeoutMs;
}

/** Translate controller state into input events, and track the head and aim poses. */
void updateInput(Context &c, XrTime time) {
	XrSpaceLocation headLocation = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
	const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	c.headValid = false;
	if (XR_SUCCEEDED(xrLocateSpace(c.viewSpace, c.space, time, &headLocation)) &&
	    (headLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		float roll;
		orientationToEuler(headLocation.pose.orientation, c.headYaw, c.headPitch, roll);
		c.headPose = headLocation.pose;
		c.headValid = true;
	}

	XrActiveActionSet active = { c.actionSet, XR_NULL_PATH };
	XrActionsSyncInfo syncInfo = xrStruct<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO);
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(c.session, &syncInfo)))
		return;

	bool selectNow[kHandCount], menuNow[kHandCount];
	for (int h = 0; h < kHandCount; h++) {
		XrActionStateGetInfo getInfo = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
		getInfo.subactionPath = c.handPath[h];
		XrActionStateBoolean state = xrStruct<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);

		getInfo.action = c.selectAction;
		selectNow[h] = XR_SUCCEEDED(xrGetActionStateBoolean(c.session, &getInfo, &state)) && state.currentState;
		getInfo.action = c.menuAction;
		menuNow[h] = XR_SUCCEEDED(xrGetActionStateBoolean(c.session, &getInfo, &state)) && state.currentState;

		if (selectNow[h] && !c.selectDown[h] && !c.buttonHeld)
			c.activeHand = h;
	}

	// Thumbstick turning: smooth, or snapped in steps. A click switches between the two.
	float stick = 0.0f;
	for (int h = 0; h < kHandCount; h++) {
		XrActionStateGetInfo getInfo = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
		getInfo.subactionPath = c.handPath[h];

		XrActionStateFloat axis = xrStruct<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
		getInfo.action = c.turnAction;
		if (XR_SUCCEEDED(xrGetActionStateFloat(c.session, &getInfo, &axis)) && axis.isActive && fabsf(axis.currentState) > fabsf(stick))
			stick = axis.currentState;

		XrActionStateBoolean click = xrStruct<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
		getInfo.action = c.turnClickAction;
		if (XR_SUCCEEDED(xrGetActionStateBoolean(c.session, &getInfo, &click)) && click.changedSinceLastSync && click.currentState) {
			c.snapTurn = !c.snapTurn;
			c.snapLatched = false;
			debug(1, "OpenXR: %s turning", c.snapTurn ? "snap" : "smooth");
		}
	}

	const uint32 now = g_system->getMillis(true);
	const float elapsed = MIN<float>((now - c.lastTurnTicks) / 1000.0f, 0.1f);
	c.lastTurnTicks = now;
	if (c.snapTurn) {
		if (!c.snapLatched && fabsf(stick) > kSnapThreshold) {
			c.pendingTurn += stick > 0.0f ? kSnapTurnAngle : -kSnapTurnAngle;
			c.snapLatched = true;
		} else if (fabsf(stick) < kSnapRelease) {
			c.snapLatched = false;
		}
	} else if (fabsf(stick) > kTurnDeadZone) {
		const float scaled = (fabsf(stick) - kTurnDeadZone) / (1.0f - kTurnDeadZone);
		c.pendingTurn += (stick > 0.0f ? 1.0f : -1.0f) * scaled * kSmoothTurnSpeed * elapsed;
	}

	// Aim ray of the active hand
	int winW, winH;
	Platform::getWindowSize(winW, winH);
	int mouseX = c.lastMouseX, mouseY = c.lastMouseY;
	bool onScreen = false;

	c.aimValid = false;
	XrSpaceLocation location = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
	if (XR_SUCCEEDED(xrLocateSpace(c.aimSpace[c.activeHand], c.space, time, &location)) &&
	    (location.locationFlags & needed) == needed) {
		float dx, dy, dz;
		forwardVector(location.pose.orientation, dx, dy, dz);
		c.aimDir[0] = dx;
		c.aimDir[1] = dy;
		c.aimDir[2] = dz;
		c.aimYaw = radToDeg(atan2f(-dx, -dz));
		c.aimPitch = radToDeg(asinf(CLIP(dy, -1.0f, 1.0f)));
		c.aimValid = true;

		// Intersect with the theatre quad
		if (c.quadAnchored && c.mirrorW > 0) {
			const float ray[3] = { dx, dy, dz };
			float normal[3] = { 0.0f, 0.0f, 1.0f };
			float right[3] = { 1.0f, 0.0f, 0.0f };
			float up[3] = { 0.0f, 1.0f, 0.0f };
			rotateVector(c.quadPose.orientation, normal);
			rotateVector(c.quadPose.orientation, right);
			rotateVector(c.quadPose.orientation, up);

			const float denominator = dot3(ray, normal);
			if (fabsf(denominator) > 0.001f) {
				const float toQuad[3] = {
					c.quadPose.position.x - location.pose.position.x,
					c.quadPose.position.y - location.pose.position.y,
					c.quadPose.position.z - location.pose.position.z
				};
				const float t = dot3(toQuad, normal) / denominator;
				const float hit[3] = {
					location.pose.position.x + t * dx - c.quadPose.position.x,
					location.pose.position.y + t * dy - c.quadPose.position.y,
					location.pose.position.z + t * dz - c.quadPose.position.z
				};
				const float height = kQuadWidth * c.mirrorH / (float)c.mirrorW;
				const float u = dot3(hit, right) / kQuadWidth + 0.5f;
				const float v = 0.5f - dot3(hit, up) / height;
				if (t > 0.0f && u >= 0.0f && u < 1.0f && v >= 0.0f && v < 1.0f) {
					mouseX = (int)(u * winW);
					mouseY = (int)(v * winH);
					onScreen = true;
				}
			}
		}
	}

	// In stereo mode the engine maps the aim ray to its own pointer
	if (onScreen && !stereoRecent(c)) {
		Platform::pushMouseMotion(mouseX, mouseY);
		c.lastMouseX = mouseX;
		c.lastMouseY = mouseY;
	}

	const bool selectActive = selectNow[c.activeHand];
	if (selectActive != c.buttonHeld && (onScreen || stereoRecent(c) || !selectActive)) {
		Platform::pushMouseButton(selectActive, mouseX, mouseY);
		c.buttonHeld = selectActive;
	}

	for (int h = 0; h < kHandCount; h++) {
		if (menuNow[h] != c.menuDown[h])
			Platform::pushEscape(menuNow[h]);
		c.selectDown[h] = selectNow[h];
		c.menuDown[h] = menuNow[h];
	}
}

bool locateViews(Context &c) {
	c.viewsValid = false;

	XrViewLocateInfo locateInfo = xrStruct<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = c.frameState.predictedDisplayTime;
	locateInfo.space = c.space;
	XrViewState viewState = xrStruct<XrViewState>(XR_TYPE_VIEW_STATE);
	for (int e = 0; e < kEyeCount; e++)
		c.views[e] = xrStruct<XrView>(XR_TYPE_VIEW);
	uint32_t count = 0;
	XR_TRY(xrLocateViews(c.session, &locateInfo, &viewState, kEyeCount, &count, c.views));
	if (count != kEyeCount || !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
		return true;

	for (int e = 0; e < kEyeCount; e++) {
		EyeView &v = c.eyeViews[e];
		orientationToEuler(c.views[e].pose.orientation, v.yaw, v.pitch, v.roll);
		v.tanLeft = tanf(c.views[e].fov.angleLeft);
		v.tanRight = tanf(c.views[e].fov.angleRight);
		v.tanDown = tanf(c.views[e].fov.angleDown);
		v.tanUp = tanf(c.views[e].fov.angleUp);
	}
	c.viewsValid = true;
	return true;
}

/** Place the theatre quad in front of where the user is looking, upright. */
void anchorQuad(Context &c) {
	c.quadAnchored = true;
	if (!c.headValid)
		return;

	const float halfYaw = c.headYaw * (float)M_PI / 360.0f;
	const float halfPitch = c.headPitch * (float)M_PI / 360.0f;
	XrQuaternionf yaw = { 0.0f, sinf(halfYaw), 0.0f, cosf(halfYaw) };
	XrQuaternionf pitch = { sinf(halfPitch), 0.0f, 0.0f, cosf(halfPitch) };
	c.quadPose.orientation = multiplyQuaternions(yaw, pitch);

	float forward[3] = { 0.0f, 0.0f, -1.0f };
	rotateVector(c.quadPose.orientation, forward);
	c.quadPose.position.x = c.headPose.position.x + forward[0] * kQuadDistance;
	c.quadPose.position.y = c.headPose.position.y + forward[1] * kQuadDistance;
	c.quadPose.position.z = c.headPose.position.z + forward[2] * kQuadDistance;
}

bool openFrame(Context &c) {
	c.frameState = xrStruct<XrFrameState>(XR_TYPE_FRAME_STATE);
	XR_TRY(xrWaitFrame(c.session, nullptr, &c.frameState));
	if (c.frameState.predictedDisplayPeriod > 0)
		c.periodMs = (uint32)(c.frameState.predictedDisplayPeriod / 1000000);
	XR_TRY(xrBeginFrame(c.session, nullptr));
	c.frameOpen = true;

	updateInput(c, c.frameState.predictedDisplayTime);
	return locateViews(c);
}

bool closeFrame(Context &c) {
	const XrCompositionLayerBaseHeader *layers[1];
	uint32_t layerCount = 0;
	XrCompositionLayerQuad quad = xrStruct<XrCompositionLayerQuad>(XR_TYPE_COMPOSITION_LAYER_QUAD);
	XrCompositionLayerProjection projection = xrStruct<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
	XrCompositionLayerProjectionView projectionViews[kEyeCount];

	if (c.frameState.shouldRender && c.stereoRendered) {
		for (int e = 0; e < kEyeCount; e++) {
			projectionViews[e] = xrStruct<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
			projectionViews[e].pose = c.views[e].pose;
			projectionViews[e].fov = c.views[e].fov;
			projectionViews[e].subImage.swapchain = c.eyeChain[e];
			projectionViews[e].subImage.imageRect.extent.width = c.eyeW;
			projectionViews[e].subImage.imageRect.extent.height = c.eyeH;
		}
		projection.space = c.space;
		projection.viewCount = kEyeCount;
		projection.views = projectionViews;
		layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&projection;
		c.quadAnchored = false;
	} else if (c.frameState.shouldRender && c.mirrorValid) {
		if (c.swapchain == XR_NULL_HANDLE || c.chainW != c.mirrorW || c.chainH != c.mirrorH) {
			if (!createSwapchain(c, c.mirrorW, c.mirrorH, c.swapchain, c.images, c.imageCount))
				return false;
			c.chainW = c.mirrorW;
			c.chainH = c.mirrorH;
		}

		uint32_t index;
		XrSwapchainImageAcquireInfo acquireInfo = xrStruct<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
		XR_TRY(xrAcquireSwapchainImage(c.swapchain, &acquireInfo, &index));
		XrSwapchainImageWaitInfo waitInfo = xrStruct<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
		waitInfo.timeout = XR_INFINITE_DURATION;
		XR_TRY(xrWaitSwapchainImage(c.swapchain, &waitInfo));

		{
			GLStateSaver saver(c.gl);
			c.gl.bindFramebuffer(GL_READ_FRAMEBUFFER, c.mirrorFBO);
			c.gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, c.chainFBO);
			c.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.images[index].image, 0);
			c.gl.blitFramebuffer(0, 0, c.mirrorW, c.mirrorH, 0, 0, c.chainW, c.chainH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			c.gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			c.gl.finish();
		}

		XrSwapchainImageReleaseInfo releaseInfo = xrStruct<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
		XR_TRY(xrReleaseSwapchainImage(c.swapchain, &releaseInfo));

		quad.space = c.space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = c.swapchain;
		quad.subImage.imageRect.extent.width = c.chainW;
		quad.subImage.imageRect.extent.height = c.chainH;
		if (!c.quadAnchored)
			anchorQuad(c);
		quad.pose = c.quadPose;
		quad.size.width = kQuadWidth;
		quad.size.height = kQuadWidth * c.chainH / (float)c.chainW;
		layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&quad;
	}

	XrFrameEndInfo endInfo = xrStruct<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
	endInfo.displayTime = c.frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = layerCount;
	endInfo.layers = layers;
	c.frameOpen = false;
	c.stereoRendered = false;
	XR_TRY(xrEndFrame(c.session, &endInfo));
	return true;
}

bool ensureEyeResources(Context &c) {
	if (c.eyeChain[0] != XR_NULL_HANDLE)
		return true;

	for (int e = 0; e < kEyeCount; e++) {
		if (!createSwapchain(c, c.eyeW, c.eyeH, c.eyeChain[e], c.eyeImages[e], c.eyeImageCount[e]))
			return false;
	}

	GLStateSaver saver(c.gl);
	GLint renderbuffer;
	c.gl.getIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
	c.gl.genRenderbuffers(1, &c.eyeDepth);
	c.gl.bindRenderbuffer(GL_RENDERBUFFER, c.eyeDepth);
	c.gl.renderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, c.eyeW, c.eyeH);
	c.gl.bindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
	c.gl.bindFramebuffer(GL_FRAMEBUFFER, c.eyeFBO);
	c.gl.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, c.eyeDepth);
	debug(1, "OpenXR: stereo resources created (%dx%d per eye)", c.eyeW, c.eyeH);
	return true;
}

} // End of anonymous namespace

void captureBackbuffer() {
	Context *c = getContext();
	if (!c)
		return;

	int w, h;
	Platform::getDrawableSize(w, h);
	if (w <= 0 || h <= 0)
		return;

	GLStateSaver saver(c->gl);
	if (w != c->mirrorW || h != c->mirrorH) {
		c->gl.bindTexture(GL_TEXTURE_2D, c->mirrorTex);
		c->gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		c->gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, c->mirrorFBO);
		c->gl.framebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->mirrorTex, 0);
		c->mirrorW = w;
		c->mirrorH = h;
	}

	c->gl.bindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	c->gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, c->mirrorFBO);
	c->gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	c->mirrorValid = true;
}

void pump() {
	Context *c = getContext();
	if (!c)
		return;

	handleEvents(*c);
	if (c->state != kRunning || !c->sessionRunning)
		return;

	if (!c->frameOpen) {
		// The engine is submitting its own frames
		if (stereoRecent(*c))
			return;

		// xrWaitFrame paces us to the headset; don't call it more often than needed
		const uint32 now = g_system->getMillis(true);
		if (now - c->lastFrameTicks + 2 < c->periodMs)
			return;

		if (!openFrame(*c)) {
			fail(*c);
			return;
		}
	}

	if (!closeFrame(*c))
		fail(*c);
	c->lastFrameTicks = g_system->getMillis(true);
}

bool beginStereo(int &width, int &height) {
	Context *c = getContext();
	if (!c)
		return false;

	handleEvents(*c);
	if (c->state != kRunning || !c->sessionRunning)
		return false;

	if (!c->frameOpen && !openFrame(*c)) {
		fail(*c);
		return false;
	}
	if (!c->frameState.shouldRender || !c->viewsValid)
		return false;
	if (!ensureEyeResources(*c)) {
		fail(*c);
		return false;
	}

	width = c->eyeW;
	height = c->eyeH;
	return true;
}

const EyeView &getEyeView(int eye) {
	return g_ctx->eyeViews[eye];
}

void bindEye(int eye) {
	Context &c = *g_ctx;

	uint32_t index;
	XrSwapchainImageAcquireInfo acquireInfo = xrStruct<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
	if (!check(xrAcquireSwapchainImage(c.eyeChain[eye], &acquireInfo, &index), "xrAcquireSwapchainImage"))
		return;
	XrSwapchainImageWaitInfo waitInfo = xrStruct<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
	waitInfo.timeout = XR_INFINITE_DURATION;
	if (!check(xrWaitSwapchainImage(c.eyeChain[eye], &waitInfo), "xrWaitSwapchainImage"))
		return;

	c.gl.getIntegerv(GL_FRAMEBUFFER_BINDING, &c.savedFBO);
	c.gl.getIntegerv(GL_VIEWPORT, c.savedViewport);

	c.gl.bindFramebuffer(GL_FRAMEBUFFER, c.eyeFBO);
	c.gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.eyeImages[eye][index].image, 0);
	c.gl.viewport(0, 0, c.eyeW, c.eyeH);
	c.gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	c.gl.depthMask(GL_TRUE);
	c.gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
	c.gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	c.eyeBound = true;
}

void endEye(int eye) {
	Context &c = *g_ctx;
	if (!c.eyeBound)
		return;

	c.gl.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
	c.gl.bindFramebuffer(GL_FRAMEBUFFER, c.savedFBO);
	c.gl.viewport(c.savedViewport[0], c.savedViewport[1], c.savedViewport[2], c.savedViewport[3]);
	c.eyeBound = false;

	XrSwapchainImageReleaseInfo releaseInfo = xrStruct<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
	check(xrReleaseSwapchainImage(c.eyeChain[eye], &releaseInfo), "xrReleaseSwapchainImage");
}

void endStereo() {
	Context &c = *g_ctx;

	c.gl.finish();
	c.stereoRendered = true;
	c.stereoEverSubmitted = true;
	c.stereoTicks = g_system->getMillis(true);
	if (!closeFrame(c))
		fail(c);
	c.lastFrameTicks = g_system->getMillis(true);
}

float consumeTurn() {
	if (!g_ctx || g_ctx->state != kRunning)
		return 0.0f;
	const float turn = g_ctx->pendingTurn;
	g_ctx->pendingTurn = 0.0f;
	return turn;
}

void leaveStereo() {
	if (g_ctx)
		g_ctx->stereoEverSubmitted = false;
}

bool isSessionRunning() {
	return g_ctx && g_ctx->state == kRunning && g_ctx->sessionRunning;
}

bool isStereoActive() {
	return g_ctx && g_ctx->state == kRunning && stereoRecent(*g_ctx);
}

bool getHeadOrientation(float &yaw, float &pitch) {
	if (!g_ctx || g_ctx->state != kRunning || !g_ctx->headValid)
		return false;
	yaw = g_ctx->headYaw;
	pitch = g_ctx->headPitch;
	return true;
}

bool projectAim(int eye, float &x, float &y) {
	if (!g_ctx || g_ctx->state != kRunning || !g_ctx->aimValid || !g_ctx->viewsValid)
		return false;

	// Rotate the aim direction into the eye space with the inverse eye orientation
	const XrQuaternionf &q = g_ctx->views[eye].pose.orientation;
	const float d[3] = { g_ctx->aimDir[0], g_ctx->aimDir[1], g_ctx->aimDir[2] };
	const float qv[3] = { -q.x, -q.y, -q.z };
	const float t[3] = {
		2.0f * (qv[1] * d[2] - qv[2] * d[1]),
		2.0f * (qv[2] * d[0] - qv[0] * d[2]),
		2.0f * (qv[0] * d[1] - qv[1] * d[0])
	};
	const float v[3] = {
		d[0] + q.w * t[0] + (qv[1] * t[2] - qv[2] * t[1]),
		d[1] + q.w * t[1] + (qv[2] * t[0] - qv[0] * t[2]),
		d[2] + q.w * t[2] + (qv[0] * t[1] - qv[1] * t[0])
	};
	if (v[2] > -0.001f)
		return false;

	const EyeView &view = g_ctx->eyeViews[eye];
	const float tx = v[0] / -v[2];
	const float ty = v[1] / -v[2];
	x = (tx - view.tanLeft) / (view.tanRight - view.tanLeft);
	y = 1.0f - (ty - view.tanDown) / (view.tanUp - view.tanDown);
	return true;
}

bool getAimOrientation(float &yaw, float &pitch) {
	if (!g_ctx || g_ctx->state != kRunning || !g_ctx->aimValid)
		return false;
	yaw = g_ctx->aimYaw;
	pitch = g_ctx->aimPitch;
	return true;
}

void shutdown() {
	if (!g_ctx)
		return;

	Context *c = g_ctx;
	g_ctx = nullptr;

	// The GL context is still current
	if (c->gl.deleteFramebuffers && c->mirrorFBO) {
		const GLuint framebuffers[] = { c->mirrorFBO, c->chainFBO, c->eyeFBO };
		c->gl.deleteFramebuffers(ARRAYSIZE(framebuffers), framebuffers);
		c->gl.deleteTextures(1, &c->mirrorTex);
		if (c->eyeDepth)
			c->gl.deleteRenderbuffers(1, &c->eyeDepth);
	}

	if (c->state == kRunning || c->session != XR_NULL_HANDLE) {
		destroySwapchain(c->swapchain, c->images, c->imageCount);
		for (int e = 0; e < kEyeCount; e++)
			destroySwapchain(c->eyeChain[e], c->eyeImages[e], c->eyeImageCount[e]);
		for (int h = 0; h < kHandCount; h++) {
			if (c->aimSpace[h] != XR_NULL_HANDLE)
				xrDestroySpace(c->aimSpace[h]);
		}
		if (c->actionSet != XR_NULL_HANDLE)
			xrDestroyActionSet(c->actionSet);
		if (c->viewSpace != XR_NULL_HANDLE)
			xrDestroySpace(c->viewSpace);
		if (c->space != XR_NULL_HANDLE)
			xrDestroySpace(c->space);
		if (c->session != XR_NULL_HANDLE)
			xrDestroySession(c->session);
		if (c->instance != XR_NULL_HANDLE)
			xrDestroyInstance(c->instance);
	}
	delete c;
}

} // End of namespace XR

#endif // USE_OPENXR
