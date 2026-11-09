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
#include <cstring>
#include <optional>
#include "hw/maple/maple_devs.h"
#include "types.h"

namespace vmufs
{
    // General pattern in this file: classes are thin wrappers.
    // They wrap either the maple device, or a pointer to a specific data structure.
    // Methods on the class allow interpreting the data. Conversion is performed "on demand".

    class VMSHeader;
    class DirectoryEntry;
    class FATBlock;
    class RootBlock;
    class DirectoryTable;

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

    // https://vmudev.net/formats.html#formats_vms_header
    class VMSHeader
    {
        FileSystem fileSystem;
        u16 blockID;
public:
        // Create from a pointer to start of the VMS header.
        VMSHeader(FileSystem fileSystem, u16 blockID) : fileSystem(fileSystem), blockID(blockID) { }

        static constexpr int HeaderSize = 0x80;
        static constexpr int IconSizeBytes = 0x200; // Size of a single icon bitmap. (4 bits per pixel)
        static constexpr int IconSizePixels = IconSizeBytes * 2;
        static constexpr int PaletteLengthBytes = 0x20;
        static constexpr int PaletteLength = 0x10;

        std::string GetVMUDescription();
        std::string GetDreamcastDescription();
        u16 GetIconCount();
        u16 GetAnimationSpeed();
        // Write 32x32 icon to 32-bit RGBA buffer of size 'IconSizePixels'
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

        u8* GetRawVMUDescription();
        u8* GetRawDreamcastDescription();
        // Get palette as array of 16 ARGB4444 values.
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
        u16 GetSuccessorBlockID(u16 blockID);
        void SetSuccessorBlockID(u16 blockID, u16 successorID);
        int CountUserFreeBlocks();
    };

    // View over the root block.
    // https://vmudev.net/filesystem.html#fs_root
    class RootBlock
    {
        u8* data;
    public:
        RootBlock(u8* data) : data(data) { }

        // Gets the custom color if this VMU is using one.
        // Note: this is meant to interop well with ImU32 from ImGui
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
