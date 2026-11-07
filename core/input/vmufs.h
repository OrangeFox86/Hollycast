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

#include <cstring>
#include <optional>
#include "types.h"
#include "rend/texconv.h"

namespace vmufs
{
    enum class FileCopyProtection : u8
    {
        NotCopyProtected = 0x80,
        CopyProtected = 0xff,
    };

    enum class FileType : u8
    {
        None = 0,
        Data = 0x33,
        Game = 0xcc,
    };

    // https://vmudev.net/formats.html#formats_vms_header
    class VMSHeader
    {
        u8* data;
public:
        // Create from a pointer to start of the VMS header.
        VMSHeader(u8* data) : data(data) { }

        static constexpr int HeaderSize = 0x80;
        static constexpr int IconSizeBytes = 0x200; // Size of a single icon bitmap. (4 bits per pixel)
        static constexpr int IconSizePixels = IconSizeBytes * 2;

        std::string GetVMUDescription();
        std::string GetDreamcastDescription();
        void ReadIconData(u32* dest);
private:
        static constexpr int Offset_VMUDescription = 0;
        static constexpr int VMUDescriptionLength = 0x10;
        static constexpr int Offset_DreamcastDescription = 0x10;
        static constexpr int DreamcastDescriptionLength = 0x20;
        static constexpr int Offset_ApplicationID = 0x30;
        static constexpr int Offset_IconCount = 0x40;
        static constexpr int Offset_AnimationSpeed = 0x42;
        static constexpr int Offset_EyecatchType = 0x44;
        static constexpr int Offset_CRC = 0x46;
        static constexpr int Offset_DataBytes = 0x48;
        static constexpr int Offset_Palette = 0x60;
        static constexpr int PaletteLengthBytes = 0x20;
        static constexpr int PaletteLength = 0x10;

        u8* GetRawVMUDescription();
        u8* GetRawDreamcastDescription();
        u16* GetRawPalette();
    };

    class DirectoryEntry
    {
        u8* data;
public:
        // Create with a pointer to the raw directory entry data.
        DirectoryEntry(u8* data) : data(data) { }

        static constexpr int Size = 0x20; // 32

        FileType GetFileType();
        void SetFileType(FileType value);

        FileCopyProtection GetFileCopyProtection();
        void SetFileCopyProtection(FileCopyProtection value);

        u16 GetStartFAT();
        void SetStartFAT(u16 value);

        std::string GetFilename();

        // TODO: date accessor

        u16 GetSizeInBlocks();
        void SetSizeInBlocks(u16 value);

        // Note: this is generally 0 for data files, 1 for game files.
        u16 GetVMSHeaderBlockOffset();
private:
        static constexpr int Offset_FileType = 0;
        static constexpr int Offset_CopyProtection = 1;
        static constexpr int Offset_StartFAT = 2;

        static constexpr int Offset_Filename = 4;
        static constexpr int FilenameLength = 12;

        static constexpr int Offset_Date = 0x10;
        static constexpr int DateLength = 8;

        static constexpr int Offset_SizeInBlocks = 0x18;
        static constexpr int Offset_VMSHeaderBlockOffset = 0x1a;

        u8* GetRawFilename();
        // Get the raw binary-coded decimal date.
        u8* GetDateBCD();
    };

    // View over the FAT block.
    // This is like a linked list of block IDs, packed into a single storage block.
    // We can lookup into it using a block ID, and get the ID of the next block of a file.
    class FATBlock
    {
        u8* data;
public:
        FATBlock(u8* data) : data(data) { }
        u16 getSuccessorBlockID(u16 blockID);
        void setSuccessorBlockID(u16 blockID, u16 successorID);
    };

    // View over the root block.
    // https://vmudev.net/filesystem.html#fs_root
    class RootBlock
    {
        u8* data;
    public:
        RootBlock(u8* data) : data(data) { }

        std::optional<u32> GetCustomColor();
        void SetCustomColor(std::optional<u32> color);
    private:
        // Get a pointer to the magic number which identifies the root block.
        u8* GetMagic();
        static constexpr int MagicLength = 0x10;
        static constexpr int Offset_UseCustomColor = MagicLength;
        static constexpr int ColorBlue = 0x11;
        static constexpr int ColorGreen = 0x12;
        static constexpr int ColorRed = 0x13;
        static constexpr int ColorAlpha = 0x14;

        static constexpr int Offset_Timestamp = 0x30;
        static constexpr int TimestampLength = 8;
        u8* GetTimestampBinary();
        u16 GetVolumeLast();
        u16 GetPartition();
        u16 GetRoot();
        u16 GetFATFirst();
        u16 GetFATSize();
        u16 GetDirLast();
        u16 GetDirSize();

        // Get the ID of a predefined icon from the Dreamcast BIOS.
        u8 GetIcon();
        void SetIcon(u8 value);

        u8 GetSort();
        u16 GetHiddenFirst();
        u16 GetHiddenSize();
        u16 GetGameFirst();
        u16 GetGameSize();
    };

    class DirectoryTable;

    static constexpr int VolumeSizeBytes = 128 * 1024; // 128kb

    // user region: where user visible files are stored.
    static constexpr int UserRegionSizeBlocks = 200;
    static constexpr int UserRegionLastBlockID = UserRegionSizeBlocks - 1;

    static constexpr int BlockSize = 0x200; // 512b
    static constexpr int BlockIDSize = 2;
    static constexpr int BlocksCount = VolumeSizeBytes / BlockSize; // 256

    static constexpr u8 DirectoryTableLastBlockID = BlocksCount - 3;
    static constexpr int DirectoryTableSizeBlocks = 13;
    static constexpr u8 DirectoryTableFirstBlockID = DirectoryTableLastBlockID - DirectoryTableSizeBlocks + 1;

    // hidden region: extra storage which is used on-demand to store user files when blocks in the user region being to malfunction.
    static constexpr int HiddenRegionSizeBlocks = BlocksCount - UserRegionSizeBlocks - DirectoryTableSizeBlocks - 1 /*root block*/ - 1 /*FAT size*/; // 41

    static constexpr u8 RootBlockID = BlocksCount - 1;
    static constexpr u8 FATBlockID = BlocksCount - 2;

    static constexpr int Magic = 0x55;

    static constexpr u16 FAT_Unallocated = 0xfffc;
    static constexpr u16 FAT_LastInFile = 0xfffa;


    class FileSystem
    {
        maple_sega_vmu* const vmuDevice;
    public:
        FileSystem(maple_sega_vmu* vmuDevice) : vmuDevice(vmuDevice) { }
        u8* GetBlock(u16 blockID);
        VMSHeader GetVMSHeader(DirectoryEntry entry);
        DirectoryTable GetDirectoryTable();
        FATBlock GetFATBlock();
        RootBlock GetRootBlock();
        bool operator!=(FileSystem& other);
    };

    // Iterable view over a VMU's directory table.
    // Note: when iterating, caller must manually filter out entries with 'Kind == None'.
    class DirectoryTable
    {
        const FileSystem fileSystem;
    public:
        DirectoryTable(FileSystem fileSystem) : fileSystem(fileSystem) { }

    private:
        static constexpr int EntriesPerBlock = BlockSize / DirectoryEntry::Size;
        static constexpr int EndIndex = DirectoryTableSizeBlocks * EntriesPerBlock;

    public:
        class Iterator
        {
            FileSystem fileSystem;
            int index;
        public:
            Iterator(FileSystem fileSystem, int index) : fileSystem(fileSystem), index(index) { }

            DirectoryEntry operator*();
            Iterator& operator++();
            bool operator!=(Iterator& other);
        };

        Iterator begin();
        Iterator end();
    };
}

// https://vmudev.net/filesystem.html
// VMU file system helpers
// See also FileSystem.cs in DreamPotato
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
    u8* VMSHeader::GetRawVMUDescription() { return &data[Offset_VMUDescription]; }
    std::string VMSHeader::GetVMUDescription()
    {
        u8* rawVMUDescription = GetRawVMUDescription();
        int len = strnlen((const char*)rawVMUDescription, VMUDescriptionLength);
        return std::string(rawVMUDescription, &rawVMUDescription[len]);
    }

    u8* VMSHeader::GetRawDreamcastDescription() { return &data[Offset_DreamcastDescription]; }
    std::string VMSHeader::GetDreamcastDescription()
    {
        u8* rawDreamcastDescription = GetRawDreamcastDescription();
        int len = strnlen((const char*)rawDreamcastDescription, DreamcastDescriptionLength);
        return std::string(rawDreamcastDescription, &rawDreamcastDescription[len]);
    }

    // Get palette as array of 16 ARGB4444 values.
    u16* VMSHeader::GetRawPalette() { return (u16*)&data[Offset_Palette]; }

    // Write 32x32 icon to 32-bit RGBA buffer of size 'IconSizePixels'
    void VMSHeader::ReadIconData(u32* dest)
    {
        u16* rawPalette = GetRawPalette();
        std::array<u32, PaletteLength> palette{};
        for (int i = 0; i < PaletteLength; i++)
            palette.at(i) = Unpacker4444_32<RGBAPacker>::unpack(rawPalette[i]);

        // TODO2: This is not working properly because
        // the icon data is not contiguous in the vmu image.
        // This code needs to start operating in terms of 'GetBlock(blockID)',
        // 'GetFATBlock().getSuccessorBlockID(blockID)', etc
        // instead of just using pointers.
        u8* sourceIconData = &data[HeaderSize];

        // Source data is 4 bits per pixel.
        // Pixel color is obtained by looking it up in the palette.
        for (int i = 0; i < IconSizeBytes; i++)
        {
            u8 source = sourceIconData[i];
            dest[i * 2] = palette.at(source >> 4 & 0xf);
            dest[i * 2 + 1] = palette.at(source & 0xf);
        }
    }
    // end class VMSHeader

    // begin class DirectoryEntry
    FileType DirectoryEntry::GetFileType() { return static_cast<FileType>(data[Offset_FileType]); }
    void DirectoryEntry::SetFileType(FileType value) { data[Offset_FileType] = static_cast<u8>(value); }

    FileCopyProtection DirectoryEntry::GetFileCopyProtection() { return static_cast<FileCopyProtection>(data[Offset_CopyProtection]); }
    void DirectoryEntry::SetFileCopyProtection(FileCopyProtection value) { data[Offset_CopyProtection] = static_cast<u8>(value); }

    u16 DirectoryEntry::GetStartFAT() { return readUInt16LittleEndian(&data[Offset_StartFAT]); }
    void DirectoryEntry::SetStartFAT(u16 value) { writeUInt16LittleEndian(&data[Offset_StartFAT], value); }

    u8* DirectoryEntry::GetRawFilename() { return &data[Offset_Filename]; }
    std::string DirectoryEntry::GetFilename()
    {
        u8* rawFilename = GetRawFilename();
        // Note: We should probably expect the filenames and VMS header descriptions to be in Shift-JIS.
        // This isn't implemented for now. But definitely should be for good Japanese game support.
        int len = strnlen((const char*)rawFilename, FilenameLength);
        return std::string(rawFilename, &rawFilename[len]);
    }

    u8* DirectoryEntry::GetDateBCD() { return &data[Offset_Date]; }

    u16 DirectoryEntry::GetSizeInBlocks() { return readUInt16LittleEndian(&data[Offset_SizeInBlocks]); }
    void DirectoryEntry::SetSizeInBlocks(u16 value) { writeUInt16LittleEndian(&data[Offset_SizeInBlocks], value); }

    u16 DirectoryEntry::GetVMSHeaderBlockOffset() { return readUInt16LittleEndian(&data[Offset_VMSHeaderBlockOffset]); }
    // end class DirectoryEntry

    // begin class FATBlock
    u16 FATBlock::getSuccessorBlockID(u16 blockID) { return readUInt16LittleEndian(&data[blockID * BlockIDSize]); }
    void FATBlock::setSuccessorBlockID(u16 blockID, u16 successorID) { writeUInt16LittleEndian(&data[blockID * BlockIDSize], successorID); }
    // end class FATBlock

    // begin class RootBlock
    // Get a pointer to the magic number which identifies the root block.
    u8* RootBlock::GetMagic() { return &data[0]; }


    // Gets the custom color if this VMU is using one.
    // Note: this is meant to interop well with ImU32 from ImGui
    std::optional<u32> RootBlock::GetCustomColor()
    {
        if (!data[Offset_UseCustomColor])
            return std::nullopt;

        return data[ColorRed] | data[ColorGreen] << 8 | data[ColorBlue] << 16 | data[ColorAlpha] << 24;
    }

    void SetCustomColor(std::optional<u32> color) { std::abort(); } // TODO: not yet implemented

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
        u16 blockID = entry.GetStartFAT() + entry.GetVMSHeaderBlockOffset();
        return VMSHeader(GetBlock(blockID));
    }

    DirectoryTable FileSystem::GetDirectoryTable() { return DirectoryTable(*this); }
    FATBlock FileSystem::GetFATBlock() { return FATBlock(GetBlock(FATBlockID)); }
    RootBlock FileSystem::GetRootBlock() { return RootBlock(GetBlock(RootBlockID)); }

    bool FileSystem::operator!=(FileSystem& other) { return vmuDevice != other.vmuDevice; }
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