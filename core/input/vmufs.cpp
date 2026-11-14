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

#include "input/vmufs.h"

#include <array>
#include <cstring>
#include <optional>
#include "hw/maple/maple_devs.h"
#include "oslib/oslib.h"
#include "rend/texconv.h"
#include "stdclass.h"
#include "types.h"

namespace vmufs
{
    u16 readUInt16LittleEndian(u8* data)
    {
        return data[0] | data[1] << 8;
    }

    void writeUInt16LittleEndian(u8* data, u16 value)
    {
        data[0] = value & 0xff;
        data[1] = value >> 8 & 0xff;
    }

    // begin class VMSHeader
    u8* VMSHeader::GetRawVMUDescription()
	{
		u8* data = fileSystem.GetBlock(blockID);
		return &data[Offset_VMUDescription];
	}

    std::string VMSHeader::GetVMUDescription()
    {
        u8* rawVMUDescription = GetRawVMUDescription();
        int len = strnlen((const char*)rawVMUDescription, VMUDescriptionLength);
        return std::string(rawVMUDescription, &rawVMUDescription[len]);
    }

    u8* VMSHeader::GetRawDreamcastDescription()
	{
		u8* data = fileSystem.GetBlock(blockID);
		return &data[Offset_DreamcastDescription];
	}

    std::string VMSHeader::GetDreamcastDescription()
    {
        u8* rawDreamcastDescription = GetRawDreamcastDescription();
        int len = strnlen((const char*)rawDreamcastDescription, DreamcastDescriptionLength);
        return std::string(rawDreamcastDescription, &rawDreamcastDescription[len]);
    }

    u16* VMSHeader::GetRawPalette()
	{
		u8* data = fileSystem.GetBlock(blockID);
		return (u16*)&data[Offset_Palette];
	}

	void readIconDataSegment(u32* dest, u8* sourceIconData, int length, std::array<u32, VMSHeader::PaletteLength>& palette)
	{
        // Source data is 4 bits per pixel.
        // Pixel color is obtained by looking it up in the palette.
        for (int i = 0; i < length; i++)
        {
            u8 source = sourceIconData[i];
            dest[i * 2] = palette.at(source >> 4 & 0xf);
            dest[i * 2 + 1] = palette.at(source & 0xf);
        }
	}

	u16 VMSHeader::GetIconCount()
	{
		u8* data = fileSystem.GetBlock(blockID);
		return readUInt16LittleEndian(&data[Offset_IconCount]);
	}

	u16 VMSHeader::GetAnimationSpeed()
	{
		u8* data = fileSystem.GetBlock(blockID);
		return readUInt16LittleEndian(&data[Offset_AnimationSpeed]);
	}

    std::array<u32, VMSHeader::IconSizePixels> VMSHeader::ReadIconData()
    {
        // https://github.com/OrangeFox86/Hollycast/issues/111
        // ICONDATA_VMS icons are not displaying properly, they need special handling.
        verify(GetIconCount() >= 1);
        u16* rawPalette = GetRawPalette();
        std::array<u32, PaletteLength> palette{};
        for (int i = 0; i < PaletteLength; i++)
            palette.at(i) = Unpacker4444_32<RGBAPacker>::unpack(rawPalette[i]);

		u8* data = fileSystem.GetBlock(blockID);
		// A single icon is split across multiple blocks.
		// Blocks may not be contiguous in the VMU image, so we have to
		// look in the FAT and get the successor block to read the last part of the icon.
		// ┌──────────────────┐
		// │ 0x0 VMS header   │
		// │ 0x80 Icon 0      │
		// │      ...         │
		// │      ...         │
		// └──────────────────┘
		// ┌──────────────────┐
		// │ 0x0  ...         │
		// │ 0x80 Icon 1      │
		// │                  │
		// │                  │
		// └──────────────────┘
		// Source data is stored as bytes with 4 bits per pixel. Destination is one array element per pixel.
		static constexpr int firstChunkSourceSize = BlockSize - HeaderSize;
		static constexpr int firstChunkDestSize = firstChunkSourceSize * 2;
        std::array<u32, IconSizePixels> dest;
		readIconDataSegment(dest.data(), &data[HeaderSize], firstChunkSourceSize, palette);

		u16 nextBlockID = fileSystem.GetFATBlock().GetSuccessorBlockID(blockID);
		readIconDataSegment(&dest.at(firstChunkDestSize), fileSystem.GetBlock(nextBlockID), HeaderSize, palette);
        return dest;
    }
    // end class VMSHeader

    // begin class DirectoryEntry
    FileType DirectoryEntry::GetFileType() { return static_cast<FileType>(data[Offset_FileType]); }
    void DirectoryEntry::SetFileType(FileType value) { data[Offset_FileType] = static_cast<u8>(value); }
    std::string DirectoryEntry::GetFileTypeString()
    {
        switch (GetFileType())
        {
            case FileType::None: return "NONE";
            case FileType::Data: return "DATA";
            case FileType::Game: return "GAME";
            default: return "?";
        }
    }

    FileCopyProtection DirectoryEntry::GetFileCopyProtection() { return static_cast<FileCopyProtection>(data[Offset_CopyProtection]); }
    void DirectoryEntry::SetFileCopyProtection(FileCopyProtection value) { data[Offset_CopyProtection] = static_cast<u8>(value); }

    u16 DirectoryEntry::GetStartFAT() { return readUInt16LittleEndian(&data[Offset_StartFAT]); }
    void DirectoryEntry::SetStartFAT(u16 value) { writeUInt16LittleEndian(&data[Offset_StartFAT], value); }

    u8* DirectoryEntry::GetRawFilename() { return &data[Offset_Filename]; }
    std::string DirectoryEntry::GetFilename()
    {
        u8* rawFilename = GetRawFilename();
        // https://github.com/OrangeFox86/Hollycast/issues/111
        // We should probably expect the filenames and VMS header descriptions to be in Shift-JIS, and convert to UTF-8 for ImGui.
        // This isn't implemented for now. But definitely should be for good Japanese game support.
        int len = strnlen((const char*)rawFilename, FilenameLength);
        return std::string(rawFilename, &rawFilename[len]);
    }

    u8* DirectoryEntry::GetRawDate() { return &data[Offset_Date]; }
    BCDDate DirectoryEntry::GetDate()
    {
        // Note: this is only fine because all the fields of BCDDate are bytes (no endianness concern).
        BCDDate* datePtr = (BCDDate*)GetRawDate();
        return *datePtr;
    }

    u16 DirectoryEntry::GetSizeInBlocks() { return readUInt16LittleEndian(&data[Offset_SizeInBlocks]); }
    void DirectoryEntry::SetSizeInBlocks(u16 value) { writeUInt16LittleEndian(&data[Offset_SizeInBlocks], value); }

    u16 DirectoryEntry::GetVMSHeaderBlockOffset() { return readUInt16LittleEndian(&data[Offset_VMSHeaderBlockOffset]); }
    // end class DirectoryEntry

    // begin class FATBlock
    u16 FATBlock::GetSuccessorBlockID(u16 blockID) { return readUInt16LittleEndian(&data[blockID * BlockIDSize]); }
    void FATBlock::SetSuccessorBlockID(u16 blockID, u16 successorID) { writeUInt16LittleEndian(&data[blockID * BlockIDSize], successorID); }
    int FATBlock::CountUserFreeBlocks()
	{
		int count = 0;
		for (int i = 0; i < UserRegionSizeBlocks; i++)
		{
			if (readUInt16LittleEndian(&data[i * 2]) == FAT_Unallocated)
				count++;
		}

		return count;
	}
    // end class FATBlock

    // begin class RootBlock
    u8* RootBlock::GetMagic() { return &data[0]; }

    std::optional<u32> RootBlock::GetCustomColor()
    {
        if (!data[Offset_UseCustomColor])
            return std::nullopt;

        return data[ColorRed] | data[ColorGreen] << 8 | data[ColorBlue] << 16 | data[ColorAlpha] << 24;
    }

    void SetCustomColor(std::optional<u32> color) { std::abort(); } // https://github.com/OrangeFox86/Hollycast/issues/111: not yet implemented

    static constexpr int Offset_Timestamp = 0x30;
    u8* RootBlock::GetTimestampBinary() { return &data[Offset_Timestamp]; }
    u16 RootBlock::GetVolumeLast() { return readUInt16LittleEndian(&data[0x40]); }
    u16 RootBlock::GetPartition() { return readUInt16LittleEndian(&data[0x42]); }
    u16 RootBlock::GetRoot() { return readUInt16LittleEndian(&data[0x44]); }
    u16 RootBlock::GetFATFirst() { return readUInt16LittleEndian(&data[0x46]); }
    u16 RootBlock::GetFATSize() { return readUInt16LittleEndian(&data[0x48]); }
    u16 RootBlock::GetDirLast() { return readUInt16LittleEndian(&data[0x4a]); }
    u16 RootBlock::GetDirSize() { return readUInt16LittleEndian(&data[0x4c]); }

    u8 RootBlock::GetIcon() { return data[0x4e]; }
    void RootBlock::SetIcon(u8 value) { data[0x4e] = value; }

    u8 RootBlock::GetSort() { return data[0x4f]; }
    u16 RootBlock::GetHiddenFirst() { return readUInt16LittleEndian(&data[0x50]); }
    u16 RootBlock::GetHiddenSize() { return readUInt16LittleEndian(&data[0x52]); }
    u16 RootBlock::GetGameFirst() { return readUInt16LittleEndian(&data[0x54]); }
    u16 RootBlock::GetGameSize() { return readUInt16LittleEndian(&data[0x56]); }
    // end class RootBlock

    // begin class FileSystem
    u8* FileSystem::GetBlock(u16 blockID) { return vmuDevice->GetBlock(blockID); }

    VMSHeader FileSystem::GetVMSHeader(DirectoryEntry entry)
    {
        // Note: it's functionally required, that a data file has offset 0, and a game file has offset 1.
        // vmi files don't store this value, so otherwise it would be impossible to round trip to vmi+vms and back.
        // If this expectation is violated, we could consider displaying an error instead of crashing. But it's a rare case.
        u16 headerBlockOffset = entry.GetVMSHeaderBlockOffset();
        u16 expectedOffset = entry.GetFileType() == FileType::Game ? 1 : 0;
        verify(headerBlockOffset == expectedOffset);
        // https://vmudev.net/filesystem.html#fs_dir
        // The docs don't make it clear whether the offset is in terms of number of 'GetSuccessor()' indirections in the FAT,
        // or if it is literally an offset to the value of the block ID.
        // In practice there is no difference in well formed VMUs, because only game files can have nonzero offset, and game files must be contiguous.
        u16 blockID = entry.GetStartFAT() + headerBlockOffset;
        return VMSHeader(*this, blockID);
    }

    DirectoryTable FileSystem::GetDirectoryTable() { return DirectoryTable(*this); }
    FATBlock FileSystem::GetFATBlock() { return FATBlock(GetBlock(FATBlockID)); }
    RootBlock FileSystem::GetRootBlock() { return RootBlock(GetBlock(RootBlockID)); }

    bool FileSystem::operator!=(FileSystem& other) { return vmuDevice != other.vmuDevice; }

	std::string FileSystem::description()
	{
		// https://github.com/OrangeFox86/Hollycast/issues/111
		// Use a richer description depending on specifics of the vmu device.
		// For example a DreamLink could maybe use the description of the link.
		// Share some "get file name" helper with storage.cpp?
		std::string path = hostfs::getVmuPath(vmuDevice->logical_port, true);
		auto slash = get_last_slash_pos(path);
		if (slash != std::string::npos && slash < path.size() - 1)
			path = path.substr(slash + 1);

		return path;
	}
    // end class FileSystem

    // begin class DirectoryTable::Iterator
    DirectoryEntry DirectoryTable::Iterator::operator*()
    {
        verify(index < EndIndex);
        // Directory blocks are visited from the end, toward the start, of the volume.
        // Entries are visited from the start, toward the end, of each block.
        // e.g. the visit order below is 'e1, e2, e3, e4, e5, e6'.
        //    ┌───────────────┐
        // b1 │ e4            │
        //    │ e5            │
        //    │ e6            │
        //    └───────────────┘
        //    ┌───────────────┐
        // b2 │ e1            │
        //    │ e2            │
        //    │ e3            │
        //    └───────────────┘
        int blockOffset = index / EntriesPerBlock;
        int blockID = DirectoryTableLastBlockID - blockOffset;
        u8* block = fileSystem.GetBlock(blockID);
        int entryID = index % EntriesPerBlock;
        int entryOffset = entryID * DirectoryEntry::Size;
        return DirectoryEntry(&block[entryOffset]);
    }

    DirectoryTable::Iterator& DirectoryTable::Iterator::operator++()
    {
        index++;
        return *this;
    }

    bool DirectoryTable::Iterator::operator!=(Iterator& other)
    {
        return fileSystem != other.fileSystem || index != other.index;
    }
    // end class DirectoryTable::Iterator

    // begin class DirectoryTable
    DirectoryTable::Iterator DirectoryTable::begin()
    {
        return DirectoryTable::Iterator(fileSystem, 0);
    }

    DirectoryTable::Iterator DirectoryTable::end()
    {
        return DirectoryTable::Iterator(fileSystem, EndIndex);
    }
    // end class DirectoryTable
}
