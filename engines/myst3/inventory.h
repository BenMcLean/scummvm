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

#ifndef INVENTORY_H_
#define INVENTORY_H_

#include "common/list.h"
#include "common/memstream.h"
#include "common/rect.h"

#include "engines/myst3/gfx.h"

#include "math/vector3d.h"

#include "video/bink_decoder.h"

namespace Myst3 {

class Myst3Engine;
class Texture;

class Inventory : public Window {
public:
	Inventory(Myst3Engine *vm);
	virtual ~Inventory();

	// Window API
	Common::Rect getPosition() const override;
	Common::Rect getOriginalPosition() const override;

	void loadFromState();
	void updateState();

	void addItem(uint16 var, bool atEnd);
	void addSaavedroChapter(uint16 var);
	void addAll();
	void removeItem(uint16 var);
	void reset();

	/** Is the mouse inside the inventory area */
	bool isMouseInside();

	/** Change the cursor when it is hovering an item */
	void updateCursor();

	void reflow();

	uint16 hoveredItem();
	void useItem(uint16 var);

	bool isEmpty() const { return _inventory.empty(); }

	void draw() override;

#ifdef USE_OPENXR
	bool isXRShown() const { return _xrShown; }

	/** Show the inventory in front of the given direction, or hide it */
	void toggleXR(float pitch, float heading);
	void closeXR() { _xrShown = false; }

	/** Draw the inventory as a panel in the panorama */
	void drawXR();
#endif

private:
	struct InventoryItem {
		uint16 var;
		Common::Rect rect;
	};

	typedef Common::List<InventoryItem> ItemList;

	struct ItemData {
		uint16 textureX;
		uint16 textureWidth;
		uint16 textureHeight;
		uint16 var;
	};

	static const ItemData _availableItems[8];
	const ItemData &getData(uint16 var);

	Myst3Engine *_vm;

	Texture *_texture;
	ItemList _inventory;
	bool _hidden;

	void initializeTexture();

	bool hasItem(uint16 var);

	void openBook(uint16 age, uint16 room, uint16 node);
	void closeAllBooks();

#ifdef USE_OPENXR
	bool _xrShown;
	Math::Vector3d _xrForward, _xrRight, _xrUp;
	Texture *_xrBackdrop;

	/** Whether the pointer is the controller ray, instead of the mouse */
	bool useXRPointer() const;
	Common::Rect getXRPanelRect() const;
	Math::Vector3d xrPanelToWorld(const Common::Rect &panel, float x, float y) const;

	/** Where the controller ray crosses the panel, in item rectangle coordinates */
	bool getXRPanelPoint(Common::Point &point) const;
#endif
};

class DragItem : public Drawable {
public:
	DragItem(Myst3Engine *vm, uint id);
	~DragItem();
	void drawOverlay() override;
	void setFrame(uint16 frame);

private:
	Myst3Engine *_vm;

	Common::SeekableReadStream *_movieStream;
	Video::BinkDecoder _bink;

	uint16 _frame;
	Texture *_texture;

	Common::Rect getPosition();
};

} // End of namespace Myst3

#endif // INVENTORY_H_
