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

#include "common/scummsys.h"

#ifdef USE_OPENXR

#include "engines/myst3/myst3.h"
#include "engines/myst3/cursor.h"
#include "engines/myst3/gfx.h"
#include "engines/myst3/inventory.h"
#include "engines/myst3/movie.h"
#include "engines/myst3/node.h"
#include "engines/myst3/scene.h"
#include "engines/myst3/state.h"

#include "backends/xr/openxr-theatre.h"

#include "math/glmath.h"
#include "math/matrix4.h"

namespace Myst3 {

static float wrap180(float angle) {
	while (angle > 180.0f)
		angle -= 360.0f;
	while (angle < -180.0f)
		angle += 360.0f;
	return angle;
}

bool Scene::isXRActive() const {
	return XR::isStereoActive();
}

bool Scene::syncXR() {
	float headYaw, headPitch;
	if (!XR::getHeadOrientation(headYaw, headPitch))
		return false;

	const float current = _vm->_state->getLookAtHeading();
	if (!_xrHaveLast) {
		// Start looking in the direction the game is facing
		_xrYawOffset = current + headYaw;
		_xrHaveLast = true;
	} else {
		// The game turned the view by itself, turn the world with it
		const float change = wrap180(current - _xrLastHeading);
		if (fabsf(change) > 0.01f)
			_xrYawOffset += change;
	}
	_xrLastHeading = current;
	_xrYawOffset = wrap180(_xrYawOffset + XR::consumeTurn());
	return true;
}

void Scene::xrAlignWorld(float referenceHeading) {
	_xrLastHeading = referenceHeading;
}

void Scene::xrAlignToGaze() {
	float headYaw, headPitch;
	if (XR::getHeadOrientation(headYaw, headPitch))
		_xrLastHeading = _xrYawOffset - headYaw;
}

void Scene::applyHeadTracking(float &pitch, float &heading) {
	if (!XR::isStereoActive() || !syncXR())
		return;

	float headYaw, headPitch;
	XR::getHeadOrientation(headYaw, headPitch);

	if (!_vm->_state->getCursorLocked()) {
		heading = _xrYawOffset - headYaw;
		pitch = headPitch;
	}
}

bool Scene::getXRPointerDirection(float &pitch, float &heading) {
	float aimYaw, aimPitch;
	if (!XR::isStereoActive() || !syncXR() || !XR::getAimOrientation(aimYaw, aimPitch))
		return false;

	heading = _xrYawOffset - aimYaw;
	pitch = aimPitch;
	return true;
}

void Myst3Engine::drawXRFrame() {
	const bool panorama = _xrWorldMovie || (_state->getViewType() == kCube && !_xrTheater);

	if (XR::consumeInventoryToggle() && panorama && !_xrWorldMovie && _scene->syncXR()) {
		float headYaw, headPitch;
		XR::getHeadOrientation(headYaw, headPitch);
		_inventory->toggleXR(headPitch, _scene->getXRYawOffset() - headYaw);
	} else if (!panorama) {
		_inventory->closeXR();
	}

	if (panorama)
		drawStereoPanorama();
	else
		XR::leaveStereo();
}

bool Myst3Engine::shouldSkipTransitionForXR() const {
	return XR::isSessionRunning() && (_xrTransitionFromPanorama || _state->getViewType() == kCube);
}

bool Myst3Engine::beginXRMovie(bool changesNode) {
	if (!XR::isSessionRunning())
		return false;

	if (_state->getViewType() == kCube && _scene->isXRActive()) {
		// Movies starting in a panorama play inside it, where they start
		_xrWorldMovie = true;
		_xrMovieFov = _state->getLookAtFOV();
		_xrKeepBackdrop = changesNode;
	} else {
		// Otherwise show them in the theatre, together with the flat node they come from
		_xrTheater = true;
	}
	return true;
}

void Myst3Engine::endXRMovie(float startHeading) {
	if (_state->getViewType() == kCube) {
		// Keep the end of the movie where the movie was shown
		if (_xrWorldMovie)
			_scene->xrAlignWorld(startHeading);
		else
			_scene->xrAlignToGaze();
	}

	delete _xrBackdrop;
	_xrBackdrop = nullptr;
	_xrKeepBackdrop = false;
	_xrWorldMovie = false;
	_xrTheater = false;
}

void Myst3Engine::drawStereoPanorama() {
	if (!_gfx->supportsXR())
		return;

	int width, height;
	if (!XR::beginStereo(width, height))
		return;

	_scene->syncXR();
	const float yawOffset = _scene->getXRYawOffset();

	static const float nearClip = 1.0f;
	static const float farClip = 10000.0f;

	float pointerPitch, pointerHeading;
	const bool hasPointer = _scene->getXRPointerDirection(pointerPitch, pointerHeading);

	_gfx->setXRTarget(width, height);
	for (int eye = 0; eye < 2; eye++) {
		const XR::EyeView &view = XR::getEyeView(eye);

		XR::bindEye(eye);

		const float heading = yawOffset - view.yaw;
		Math::Matrix4 projection = Math::makeFrustumMatrix(
				view.tanLeft * nearClip, view.tanRight * nearClip,
				view.tanDown * nearClip, view.tanUp * nearClip,
				nearClip, farClip);
		Math::Matrix4 modelView(180.0f - heading, view.pitch, view.roll, Math::EO_YXZ);
		_gfx->setupCameraXR(projection, modelView);

		// While a movie plays inside the panorama, everything around it is black
		if (!_xrWorldMovie && _state->getViewType() == kCube && _node)
			_gfx->renderDrawable(_node, _scene);

		if (!_xrBackdrop) {
			for (int i = _movies.size() - 1; i >= 0; i--)
				_gfx->renderDrawable(_movies[i], _scene);
		}

		for (uint i = 0; i < _drawables.size(); i++) {
			if (_drawables[i]->is3D())
				_gfx->renderDrawable(_drawables[i], _scene);
		}

		_inventory->drawXR();

		float pointerX, pointerY;
		if (hasPointer && _cursor->isVisible() && XR::projectAim(eye, pointerX, pointerY)) {
			const int x = (int)(pointerX * width);
			const int y = (int)(pointerY * height);

			// About three degrees across
			_gfx->selectTargetWindow(nullptr, false, false);
			_cursor->drawAt(Common::Point(x, y), width / 1000.0f);
		}

		XR::endEye(eye);
	}
	_gfx->setXRTarget(0, 0);
	XR::endStereo();

	// Restore the regular camera, used for the desktop window
	_gfx->setupCameraPerspective(_state->getLookAtPitch(), _state->getLookAtHeading(), _state->getLookAtFOV());
}

} // End of namespace Myst3

#endif // USE_OPENXR
