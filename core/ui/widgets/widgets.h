/*
	Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.

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

#include "imgui.h"

// Placeholder widget library
// This is a minimal implementation to allow compilation
// Full widget system implementation is pending

namespace Widgets {

// Placeholder for BoolOption namespace
namespace BoolOption {
	// Placeholder function - to be implemented
	inline bool Option(const char* label, const char* tooltip, bool& value, bool disabled = false) {
		if (disabled) ImGui::BeginDisabled();
		bool clicked = ImGui::Checkbox(label, &value);
		if (disabled) ImGui::EndDisabled();
		if (tooltip && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("%s", tooltip);
		}
		return clicked;
	}
}

} // namespace Widgets
