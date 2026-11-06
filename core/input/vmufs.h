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
#include <cstring>
#include <optional>
#include <types.h>

// https://vmudev.net/filesystem.html
// See also FileSystem.cs in DreamPotato
namespace vmufs
{
    uint16_t readUInt16LittleEndian(uint8_t* data)
    {
        return data[0] | data[1] << 8;
    }

    void writeUInt16LittleEndian(uint8_t* data, uint16_t value)
    {
        data[0] = value & 0xff;
        data[1] = value >> 8 & 0xff;
    }

    class FileSystem;

    enum class FileCopyProtection : uint8_t
    {
        NotCopyProtected = 0x80,
        CopyProtected = 0xff,
    };

    enum class FileType : uint8_t
    {
        None = 0,
        Data = 0x33,
        Game = 0xcc,
    };

    // https://vmudev.net/formats.html#formats_vms_header
    class VMSHeader
    {
        uint8_t* data;
        // TODO2: add constructor/accessors to get the vms header data.
        // Expose the icon bitmaps in some way which is friendly to imgui's texture format
public:
        // Create from a pointer to start of the VMS header.
        VMSHeader(uint8_t* data) : data(data) { }

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

        uint8_t* GetRawVMUDescription() { return &data[Offset_VMUDescription]; }
        void SetRawVMUDescription(uint8_t* value) { memcpy(&data[Offset_VMUDescription], value, VMUDescriptionLength); }
        std::string GetVMUDescription()
        {
            uint8_t* rawVMUDescription = GetRawVMUDescription();
            int len = strnlen((const char*)rawVMUDescription, VMUDescriptionLength);
            return std::string(rawVMUDescription, &rawVMUDescription[len]);
        }

        uint8_t* GetRawDreamcastDescription() { return &data[Offset_DreamcastDescription]; }
        void SetRawDreamcastDescription(uint8_t* value) { memcpy(&data[Offset_DreamcastDescription], value, DreamcastDescriptionLength); }
        std::string GetDreamcastDescription()
        {
            uint8_t* rawDreamcastDescription = GetRawDreamcastDescription();
            int len = strnlen((const char*)rawDreamcastDescription, DreamcastDescriptionLength);
            return std::string(rawDreamcastDescription, &rawDreamcastDescription[len]);
        }

        // Write 32x32 icon to ARGB buffer
        // TODO2: implement. Need to be able to read the palette, translate the colors etc
        void ReadIconData(uint32_t* dest) { }
    };

    class DirectoryEntry
    {
        uint8_t* data;
public:
        // Create with a pointer to the raw directory entry data.
        DirectoryEntry(uint8_t* data) : data(data) { }

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
        void SetFileType(FileType value) { data[Offset_FileType] = static_cast<uint8_t>(value); }

        FileCopyProtection GetFileCopyProtection() { return static_cast<FileCopyProtection>(data[Offset_CopyProtection]); }
        void SetFileCopyProtection(FileCopyProtection value) { data[Offset_CopyProtection] = static_cast<uint8_t>(value); }

        uint16_t GetStartFAT() { return readUInt16LittleEndian(&data[Offset_StartFAT]); }
        void SetStartFAT(uint16_t value) { writeUInt16LittleEndian(&data[Offset_StartFAT], value); }

        uint8_t* GetRawFilename() { return &data[Offset_Filename]; }
        void SetRawFilename(uint8_t* value) { memcpy(&data[Offset_Filename], value, FilenameLength); }
        std::string GetFilename()
        {
            uint8_t* rawFilename = GetRawFilename();
            // Note: We should probably expect the filenames and VMS header descriptions to be in Shift-JIS.
            // This isn't implemented for now. But definitely should be for good Japanese game support.
            int len = strnlen((const char*)rawFilename, FilenameLength);
            return std::string(rawFilename, &rawFilename[len]);
        }

        // Get the raw binary-coded decimal date.
        uint8_t* GetDateBCD() { return &data[Offset_Date]; }
        void SetDateBCD(uint8_t* value) { memcpy(&data[Offset_Date], value, DateLength); }

        uint16_t GetSizeInBlocks() { return readUInt16LittleEndian(&data[Offset_SizeInBlocks]); }
        void SetSizeInBlocks(uint16_t value) { writeUInt16LittleEndian(&data[Offset_SizeInBlocks], value); }

        // Note: this is *usually* 0 for data files, 1 for game files.
        uint16_t GetVMSHeaderBlockOffset() { return readUInt16LittleEndian(&data[Offset_VMSHeaderBlockOffset]); }
        void SetVMSHeaderBlockOffset(uint16_t value) { writeUInt16LittleEndian(&data[Offset_VMSHeaderBlockOffset], value); }
    };

    static constexpr int VolumeSizeBytes = 128 * 1024; // 128kb

    // user region: where user visible files are stored.
    static constexpr int UserRegionSizeBlocks = 200;
    static constexpr int UserRegionLastBlockID = UserRegionSizeBlocks - 1;

    static constexpr int BlockSize = 0x200; // 512b
    static constexpr int BlockIDSize = 2;
    static constexpr int BlocksCount = VolumeSizeBytes / BlockSize; // 256

    static constexpr uint8_t DirectoryTableLastBlockID = BlocksCount - 3;
    static constexpr int DirectoryTableSizeBlocks = 13;
    static constexpr uint8_t DirectoryTableFirstBlockID = DirectoryTableLastBlockID - DirectoryTableSizeBlocks + 1;

    // hidden region: extra storage which is used on-demand to store user files when blocks in the user region being to malfunction.
    static constexpr int HiddenRegionSizeBlocks = BlocksCount - UserRegionSizeBlocks - DirectoryTableSizeBlocks - 1 /*root block*/ - 1 /*FAT size*/; // 41

    static constexpr uint8_t RootBlockID = BlocksCount - 1;
    static constexpr uint8_t FATBlockID = BlocksCount - 2;

    static constexpr int Magic = 0x55;

    static constexpr uint16_t FAT_Unallocated = 0xfffc;
    static constexpr uint16_t FAT_LastInFile = 0xfffa;

    // View over the FAT block.
    // This is like a linked list of block IDs, packed into a single storage block.
    // We can lookup into it using a block ID, and get the ID of the next block of a file.
    class FATBlock
    {
        uint8_t* data;
public:
        FATBlock(uint8_t* data) : data(data) { }
        uint16_t getSuccessorBlockID(uint16_t blockID) { return readUInt16LittleEndian(&data[blockID * BlockIDSize]); }
        void setSuccessorBlockID(uint16_t blockID, uint16_t value) { writeUInt16LittleEndian(&data[blockID * BlockIDSize], value); }
    };

    // View over the root block.
    class RootBlock
    {
        uint8_t* data;
public:
        RootBlock(uint8_t* data) : data(data) { }

        // Get a pointer to the magic number which identifies the root block.
        uint8_t* GetMagic() { return &data[0]; }

        static constexpr int MagicLength = 0x10;
        static constexpr int Offset_UseCustomColor = MagicLength;
        static constexpr int ColorBlue = 0x11;
        static constexpr int ColorGreen = 0x12;
        static constexpr int ColorRed = 0x13;
        static constexpr int ColorAlpha = 0x14;

        // Gets the custom color if this VMU is using one.
        // Note: this is meant to interop well with ImU32 from ImGui
        std::optional<uint32_t> GetCustomColor()
        {
            if (!data[Offset_UseCustomColor])
                return std::nullopt;

            return data[ColorRed] | data[ColorGreen] << 8 | data[ColorBlue] << 16 | data[ColorAlpha] << 24;
        }

        static constexpr int Offset_Timestamp = 0x30;
        uint8_t* GetTimestampBinary() { return &data[Offset_Timestamp]; }

        uint16_t GetVolumeLast() { return readUInt16LittleEndian(&data[0x40]); }
        void SetVolumeLast(uint16_t value) { return writeUInt16LittleEndian(&data[0x40], value); }

        uint16_t GetPartition() { return readUInt16LittleEndian(&data[0x42]); }
        void SetPartition(uint16_t value) { return writeUInt16LittleEndian(&data[0x42], value); }

        uint16_t GetRoot() { return readUInt16LittleEndian(&data[0x44]); }
        void SetRoot(uint16_t value) { return writeUInt16LittleEndian(&data[0x44], value); }

        uint16_t GetFATFirst() { return readUInt16LittleEndian(&data[0x46]); }
        void SetFATFirst(uint16_t value) { return writeUInt16LittleEndian(&data[0x46], value); }

        uint16_t GetFATSize() { return readUInt16LittleEndian(&data[0x48]); }
        void SetFATSize(uint16_t value) { return writeUInt16LittleEndian(&data[0x48], value); }

        uint16_t GetDirLast() { return readUInt16LittleEndian(&data[0x4a]); }
        void SetDirLast(uint16_t value) { return writeUInt16LittleEndian(&data[0x4a], value); }

        uint16_t GetDirSize() { return readUInt16LittleEndian(&data[0x4c]); }
        void SetDirSize(uint16_t value) { return writeUInt16LittleEndian(&data[0x4c], value); }

        uint8_t GetIcon() { return data[0x4e]; }
        void SetIcon(uint8_t value) { data[0x4e] = value; }

        uint8_t GetSort() { return data[0x4f]; }
        void SetSort(uint8_t value) { data[0x4f] = value; }

        uint16_t GetHiddenFirst() { return readUInt16LittleEndian(&data[0x50]); }
        void SetHiddenFirst(uint16_t value) { return writeUInt16LittleEndian(&data[0x50], value); }

        uint16_t GetHiddenSize() { return readUInt16LittleEndian(&data[0x52]); }
        void SetHiddenSize(uint16_t value) { return writeUInt16LittleEndian(&data[0x52], value); }

        uint16_t GetGameFirst() { return readUInt16LittleEndian(&data[0x54]); }
        void SetGameFirst(uint16_t value) { return writeUInt16LittleEndian(&data[0x54], value); }

        uint16_t GetGameSize() { return readUInt16LittleEndian(&data[0x56]); }
        void SetGameSize(uint16_t value) { return writeUInt16LittleEndian(&data[0x56], value); }
    };

    // Iterable view over a VMU's directory table.
    // Note: when iterating, caller must manually filter out entries with 'Kind == None'.
    class DirectoryTable
    {
        uint8_t* data;
    public:
        // Note: a pointer to the start of the volume is expected.
        // Not to the start of the directory table.
        DirectoryTable(uint8_t* data) : data(data) { }

        static constexpr int EntriesPerBlock = BlockSize / DirectoryEntry::Size;
        static constexpr int EndIndex = DirectoryTableSizeBlocks * EntriesPerBlock;

        class Iterator
        {
            uint8_t* data;
            int index;
        public:
            Iterator(uint8_t* data, int index) : data(data), index(index) { }

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
                int entryID = index % EntriesPerBlock;
                int finalOffset = blockID * BlockSize + entryID * DirectoryEntry::Size;
                return DirectoryEntry(&data[finalOffset]);
            }

            Iterator& operator ++()
            {
                index++;
                return *this;
            }

            bool operator !=(Iterator& other)
            {
                return data != other.data || index != other.index;
            }
        };

        Iterator begin()
        {
            return Iterator(data, 0);
        }

        Iterator end()
        {
            return Iterator(data, EndIndex);
        }
    };

    class FileSystem
    {
        uint8_t* data;
    public:
        // TODO: this is a pointer to the VMU image and requires the whole VMU to be read in advance.
        // We should instead expose 'ReadBlock' and 'WriteBlock' which delegate to a maple_sega_vmu.
        // This should let the functionality just work with external real hardware VMUs, etc.
        // with only the minimally necessary I/O actually being performed.
        FileSystem(uint8_t* data) : data(data) { }

        uint8_t* GetBlock(uint16_t blockID) { return &data[blockID * BlockSize]; }

        VMSHeader GetVMSHeader(DirectoryEntry entry)
        {
            uint16_t blockID = entry.GetStartFAT() + entry.GetVMSHeaderBlockOffset();
            return VMSHeader(GetBlock(blockID));
        }

        DirectoryTable GetDirectoryTable() { return DirectoryTable(data); }
        FATBlock GetFATBlock() { return FATBlock(GetBlock(FATBlockID)); }
        RootBlock GetRootBlock() { return RootBlock(GetBlock(RootBlockID)); }
    };
}