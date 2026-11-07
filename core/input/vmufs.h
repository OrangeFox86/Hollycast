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
#include <types.h>
#include "rend/texconv.h"

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

    class DirectoryTable;

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
        static constexpr int HeaderSize = 0x80;
        static constexpr int IconSizeBytes = 0x200; // Size of a single icon bitmap. (4 bits per pixel)
        static constexpr int IconSizePixels = IconSizeBytes * 2;

        u8* GetRawVMUDescription() { return &data[Offset_VMUDescription]; }
        void SetRawVMUDescription(u8* value) { memcpy(&data[Offset_VMUDescription], value, VMUDescriptionLength); }
        std::string GetVMUDescription()
        {
            u8* rawVMUDescription = GetRawVMUDescription();
            int len = strnlen((const char*)rawVMUDescription, VMUDescriptionLength);
            return std::string(rawVMUDescription, &rawVMUDescription[len]);
        }

        u8* GetRawDreamcastDescription() { return &data[Offset_DreamcastDescription]; }
        void SetRawDreamcastDescription(u8* value) { memcpy(&data[Offset_DreamcastDescription], value, DreamcastDescriptionLength); }
        std::string GetDreamcastDescription()
        {
            u8* rawDreamcastDescription = GetRawDreamcastDescription();
            int len = strnlen((const char*)rawDreamcastDescription, DreamcastDescriptionLength);
            return std::string(rawDreamcastDescription, &rawDreamcastDescription[len]);
        }

        // Get palette as array of 16 ARGB4444 values.
        u16* GetRawPalette() { return (u16*)&data[Offset_Palette]; }
        u16* SetRawPalette(u16* value) { memcpy(&data[Offset_Palette], value, PaletteLengthBytes); }

        // Write 32x32 icon to 32-bit RGBA buffer of size 'IconSizePixels'
        void ReadIconData(u32* dest)
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
    };

    class DirectoryEntry
    {
        u8* data;
public:
        // Create with a pointer to the raw directory entry data.
        DirectoryEntry(u8* data) : data(data) { }

        static constexpr int Size = 0x20; // 32
        static constexpr int Offset_FileType = 0;
        static constexpr int Offset_CopyProtection = 1;
        static constexpr int Offset_StartFAT = 2;

        static constexpr int Offset_Filename = 4;
        static constexpr int FilenameLength = 12;

        static constexpr int Offset_Date = 0x10;
        static constexpr int DateLength = 8;

        static constexpr int Offset_SizeInBlocks = 0x18;
        static constexpr int Offset_VMSHeaderBlockOffset = 0x1a;

        FileType GetFileType() { return static_cast<FileType>(data[Offset_FileType]); }
        void SetFileType(FileType value) { data[Offset_FileType] = static_cast<u8>(value); }

        FileCopyProtection GetFileCopyProtection() { return static_cast<FileCopyProtection>(data[Offset_CopyProtection]); }
        void SetFileCopyProtection(FileCopyProtection value) { data[Offset_CopyProtection] = static_cast<u8>(value); }

        u16 GetStartFAT() { return readUInt16LittleEndian(&data[Offset_StartFAT]); }
        void SetStartFAT(u16 value) { writeUInt16LittleEndian(&data[Offset_StartFAT], value); }

        u8* GetRawFilename() { return &data[Offset_Filename]; }
        void SetRawFilename(u8* value) { memcpy(&data[Offset_Filename], value, FilenameLength); }
        std::string GetFilename()
        {
            u8* rawFilename = GetRawFilename();
            // Note: We should probably expect the filenames and VMS header descriptions to be in Shift-JIS.
            // This isn't implemented for now. But definitely should be for good Japanese game support.
            int len = strnlen((const char*)rawFilename, FilenameLength);
            return std::string(rawFilename, &rawFilename[len]);
        }

        // Get the raw binary-coded decimal date.
        u8* GetDateBCD() { return &data[Offset_Date]; }
        void SetDateBCD(u8* value) { memcpy(&data[Offset_Date], value, DateLength); }

        u16 GetSizeInBlocks() { return readUInt16LittleEndian(&data[Offset_SizeInBlocks]); }
        void SetSizeInBlocks(u16 value) { writeUInt16LittleEndian(&data[Offset_SizeInBlocks], value); }

        // Note: this is *usually* 0 for data files, 1 for game files.
        u16 GetVMSHeaderBlockOffset() { return readUInt16LittleEndian(&data[Offset_VMSHeaderBlockOffset]); }
        void SetVMSHeaderBlockOffset(u16 value) { writeUInt16LittleEndian(&data[Offset_VMSHeaderBlockOffset], value); }
    };

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

    // View over the FAT block.
    // This is like a linked list of block IDs, packed into a single storage block.
    // We can lookup into it using a block ID, and get the ID of the next block of a file.
    class FATBlock
    {
        u8* data;
public:
        FATBlock(u8* data) : data(data) { }
        u16 getSuccessorBlockID(u16 blockID) { return readUInt16LittleEndian(&data[blockID * BlockIDSize]); }
        void setSuccessorBlockID(u16 blockID, u16 successorID) { writeUInt16LittleEndian(&data[blockID * BlockIDSize], successorID); }
    };

    // View over the root block.
    class RootBlock
    {
        u8* data;
public:
        RootBlock(u8* data) : data(data) { }

        // Get a pointer to the magic number which identifies the root block.
        u8* GetMagic() { return &data[0]; }

        static constexpr int MagicLength = 0x10;
        static constexpr int Offset_UseCustomColor = MagicLength;
        static constexpr int ColorBlue = 0x11;
        static constexpr int ColorGreen = 0x12;
        static constexpr int ColorRed = 0x13;
        static constexpr int ColorAlpha = 0x14;

        // Gets the custom color if this VMU is using one.
        // Note: this is meant to interop well with ImU32 from ImGui
        std::optional<u32> GetCustomColor()
        {
            if (!data[Offset_UseCustomColor])
                return std::nullopt;

            return data[ColorRed] | data[ColorGreen] << 8 | data[ColorBlue] << 16 | data[ColorAlpha] << 24;
        }

        static constexpr int Offset_Timestamp = 0x30;
        u8* GetTimestampBinary() { return &data[Offset_Timestamp]; }

        u16 GetVolumeLast() { return readUInt16LittleEndian(&data[0x40]); }
        void SetVolumeLast(u16 value) { return writeUInt16LittleEndian(&data[0x40], value); }

        u16 GetPartition() { return readUInt16LittleEndian(&data[0x42]); }
        void SetPartition(u16 value) { return writeUInt16LittleEndian(&data[0x42], value); }

        u16 GetRoot() { return readUInt16LittleEndian(&data[0x44]); }
        void SetRoot(u16 value) { return writeUInt16LittleEndian(&data[0x44], value); }

        u16 GetFATFirst() { return readUInt16LittleEndian(&data[0x46]); }
        void SetFATFirst(u16 value) { return writeUInt16LittleEndian(&data[0x46], value); }

        u16 GetFATSize() { return readUInt16LittleEndian(&data[0x48]); }
        void SetFATSize(u16 value) { return writeUInt16LittleEndian(&data[0x48], value); }

        u16 GetDirLast() { return readUInt16LittleEndian(&data[0x4a]); }
        void SetDirLast(u16 value) { return writeUInt16LittleEndian(&data[0x4a], value); }

        u16 GetDirSize() { return readUInt16LittleEndian(&data[0x4c]); }
        void SetDirSize(u16 value) { return writeUInt16LittleEndian(&data[0x4c], value); }

        u8 GetIcon() { return data[0x4e]; }
        void SetIcon(u8 value) { data[0x4e] = value; }

        u8 GetSort() { return data[0x4f]; }
        void SetSort(u8 value) { data[0x4f] = value; }

        u16 GetHiddenFirst() { return readUInt16LittleEndian(&data[0x50]); }
        void SetHiddenFirst(u16 value) { return writeUInt16LittleEndian(&data[0x50], value); }

        u16 GetHiddenSize() { return readUInt16LittleEndian(&data[0x52]); }
        void SetHiddenSize(u16 value) { return writeUInt16LittleEndian(&data[0x52], value); }

        u16 GetGameFirst() { return readUInt16LittleEndian(&data[0x54]); }
        void SetGameFirst(u16 value) { return writeUInt16LittleEndian(&data[0x54], value); }

        u16 GetGameSize() { return readUInt16LittleEndian(&data[0x56]); }
        void SetGameSize(u16 value) { return writeUInt16LittleEndian(&data[0x56], value); }
    };

    class FileSystem
    {
        maple_sega_vmu* const vmuDevice;
    public:
        FileSystem(maple_sega_vmu* vmuDevice) : vmuDevice(vmuDevice) { }

        u8* GetBlock(u16 blockID) { return vmuDevice->GetBlock(blockID); }

        VMSHeader GetVMSHeader(DirectoryEntry entry)
        {
            u16 blockID = entry.GetStartFAT() + entry.GetVMSHeaderBlockOffset();
            return VMSHeader(GetBlock(blockID));
        }

        DirectoryTable GetDirectoryTable();
        FATBlock GetFATBlock() { return FATBlock(GetBlock(FATBlockID)); }
        RootBlock GetRootBlock() { return RootBlock(GetBlock(RootBlockID)); }

        bool operator !=(FileSystem& other) { return vmuDevice != other.vmuDevice; }
    };

    // Iterable view over a VMU's directory table.
    // Note: when iterating, caller must manually filter out entries with 'Kind == None'.
    class DirectoryTable
    {
        const FileSystem fileSystem;
    public:
        DirectoryTable(FileSystem fileSystem) : fileSystem(fileSystem) { }

        static constexpr int EntriesPerBlock = BlockSize / DirectoryEntry::Size;
        static constexpr int EndIndex = DirectoryTableSizeBlocks * EntriesPerBlock;

        class Iterator
        {
            FileSystem fileSystem;
            int index;
        public:
            Iterator(FileSystem fileSystem, int index) : fileSystem(fileSystem), index(index) { }

            DirectoryEntry operator *()
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

            Iterator& operator ++()
            {
                index++;
                return *this;
            }

            bool operator !=(Iterator& other)
            {
                return fileSystem != other.fileSystem || index != other.index;
            }
        };

        Iterator begin()
        {
            return Iterator(fileSystem, 0);
        }

        Iterator end()
        {
            return Iterator(fileSystem, EndIndex);
        }
    };

    DirectoryTable FileSystem::GetDirectoryTable() { return DirectoryTable(*this); }
}