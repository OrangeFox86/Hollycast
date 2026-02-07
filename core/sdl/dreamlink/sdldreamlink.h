/*
	Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
	https://github.com/OrangeFox86/Hollycast

	Hollycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Hollycast is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Hollycast.  If not, see <https://www.gnu.org/licenses/>.
*/

#pragma once

#include <input/dreamlink.h>

class SDLDreamLink : public BaseDreamLink
{
public:
    //! Constructor
    //! @param[in] storageSupported True if this DreamLink supports physical VMU memory
    inline SDLDreamLink(bool storageSupported) : BaseDreamLink(storageSupported)
    {}

    //! Destructor (virtual, default)
    virtual ~SDLDreamLink() = default;

	//! Changes the selected maple port is changed by the user
	virtual void changeBus(int newBus) = 0;

	//! Called once the gamepad is registered - the dreamlink should register itself
	virtual void registered() = 0;
};
