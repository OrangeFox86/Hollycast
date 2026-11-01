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

    class DirectoryEntry
    {
        static constexpr int Size = 0x20; // 32
        static constexpr int Offset_FileType = 0;
        static constexpr int Offset_CopyProtection = 1;
        static constexpr int Offset_StartFAT = 2;

        static constexpr int Offset_Filename = 4;
        static constexpr int FilenameLength = 12;

        static constexpr int Offset_Date = 0x10;
        static constexpr int DateLength = 8;

        static constexpr int Offset_SizeInBlocks = 0x18;
        static constexpr int Offset_VmsHeaderBlockOffset = 0x1a;

        uint8_t* data;
public:
        DirectoryEntry(uint8_t* data) : data(data) { }

        FileType GetFileType() { return static_cast<FileType>(data[Offset_FileType]); }
        void SetFileType(FileType value) { data[Offset_FileType] = static_cast<uint8_t>(value); }

        FileCopyProtection GetFileCopyProtection() { return static_cast<FileCopyProtection>(data[Offset_CopyProtection]); }
        void SetFileCopyProtection(FileCopyProtection value) { data[Offset_CopyProtection] = static_cast<uint8_t>(value); }

        uint16_t GetStartFAT() { return readUInt16LittleEndian(&data[Offset_StartFAT]); }
        void SetStartFAT(uint16_t value) { writeUInt16LittleEndian(&data[Offset_StartFAT], value); }

        uint8_t* GetFilename() { return &data[Offset_Filename]; }
        void SetFilename(uint8_t* value) { memcpy(&data[Offset_Filename], value, FilenameLength); }

        // Get the raw binary-coded decimal date.
        uint8_t* GetDateBcd() { return &data[Offset_Date]; }
        void SetDateBcd(uint8_t* value) { memcpy(&data[Offset_Date], value, DateLength); }
    };

    static constexpr int VolumeSizeBytes = 128 * 1024; // 128kb

    // user region: where user visible files are stored.
    static constexpr int UserRegionSizeBlocks = 200;
    static constexpr int UserRegionLastblockID = UserRegionSizeBlocks - 1;

    static constexpr int BlockSize = 0x200; // 512b
    static constexpr int blockIDSize = 2;
    static constexpr int BlocksCount = VolumeSizeBytes / BlockSize; // 256

    static constexpr uint8_t DirectoryTableLastblockID = BlocksCount - 3;
    static constexpr int DirectoryTableSizeBlocks = 13;
    static constexpr uint8_t DirectoryTableFirstblockID = DirectoryTableLastblockID - DirectoryTableSizeBlocks + 1;

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
        uint16_t getSuccessorblockID(uint16_t blockID) { return readUInt16LittleEndian(&data[blockID * blockIDSize]); }
        void setSuccessorblockID(uint16_t blockID, uint16_t value) { writeUInt16LittleEndian(&data[blockID * blockIDSize], value); }
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

    class FileSystem
    {
        uint8_t* data;
    public:
        FileSystem(uint8_t* data) : data(data) { }

        uint8_t* GetBlock(uint16_t blockID) { return &data[blockID * BlockSize]; }

        FATBlock GetFATBlock() { return FATBlock(GetBlock(FATBlockID)); }
        RootBlock GetRootBlock() { return RootBlock(GetBlock(RootBlockID)); }
    };
}