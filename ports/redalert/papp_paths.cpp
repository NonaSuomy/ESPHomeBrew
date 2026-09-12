// Paths and file lookup for the Red Alert PAPP (replaces common/paths_posix.cpp
// and common/file_posix.cpp). Program, data and user files all live in one
// folder on the SD card; REDALERT.INI there can still override them.
#include "papp_port.h"

#include "common/file.h"
#include "common/paths.h"

#include <string.h>

const char* PathsClass::Program_Path()
{
    if (ProgramPath.empty()) {
        ProgramPath = PAPP_RA_DATA_DIR;
    }
    return ProgramPath.c_str();
}

const char* PathsClass::Data_Path()
{
    if (DataPath.empty()) {
        DataPath = PAPP_RA_DATA_DIR;
    }
    return DataPath.c_str();
}

const char* PathsClass::User_Path()
{
    if (UserPath.empty()) {
        UserPath = PAPP_RA_DATA_DIR;
    }
    return UserPath.c_str();
}

bool PathsClass::Create_Directory(const char*)
{
    return true;  // the data folder already exists; the loader has no mkdir
}

bool PathsClass::Is_Absolute(const char* path)
{
    return path != nullptr && path[0] == '/';
}

std::string PathsClass::Concatenate_Paths(const char* path1, const char* path2)
{
    return std::string(path1) + SEP + path2;
}

std::string PathsClass::Get_Filename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return std::string(slash != nullptr ? slash + 1 : path);
}

std::string PathsClass::Argv_Path(const char*)
{
    return PAPP_RA_DATA_DIR;
}

// There is no directory listing through the loader. Find_First() then finds
// nothing, and Resolve_File() keeps names as they are, which is right on FAT
// (case-insensitive). What is lost: scanning for optional SC*.MIX files and
// listing saved games; the game still loads fixed names.
class Find_File_Data_PAPP : public Find_File_Data
{
public:
    const char* GetName() const override
    {
        return nullptr;
    }
    unsigned int GetTime() const override
    {
        return 0;
    }
    bool FindFirst(const char*) override
    {
        return false;
    }
    bool FindNext() override
    {
        return false;
    }
    void Close() override
    {
    }
};

Find_File_Data* Find_File_Data::CreateFindData()
{
    return new Find_File_Data_PAPP();
}
