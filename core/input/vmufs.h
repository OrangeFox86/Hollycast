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

#include <cstdint>
#include <cstddef>
#include <array>

namespace vmufs
{

enum class FileType : std::uint8_t
{
    None = 0,
    Data = 0x33,
    Game = 0xcc,
};

enum class FileCopyProtection : std::uint8_t
{
    NotCopyProtected = 0x80,
    CopyProtected = 0xff,
};

// TODO2: populate from pointer, handle endianness.
struct DirectoryEntry
{
    FileType fileType;
    FileCopyProtection copyProtection;
    std::uint16_t startFAT;
    std::array<char, 12> filename;
    std::array<std::uint8_t, 8> dateBCD;
    std::uint16_t sizeInBlocks;
    std::uint16_t vmsHeaderBlockOffset;
};

struct DirectoryTable
{
    struct Iterator
    {

    };

    Iterator begin() const;
    Iterator end() const;
};

constexpr int BlockSize = 0x200;

struct FATBlock
{
    std::array<uint8_t, BlockSize> rawBlock;
};

struct RootBlock
{

};

// View of a VMU's contents which exposes ability to list files, import/export file contents, etc.
class VMU
{
};

}