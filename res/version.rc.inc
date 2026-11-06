//  Version resource shared by both executables.

VS_VERSION_INFO VERSIONINFO
 FILEVERSION XWW_VERSION_BINARY
 PRODUCTVERSION XWW_VERSION_BINARY
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS VOS_NT_WINDOWS32
 FILETYPE VFT_APP
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "xwwraw"
            VALUE "FileDescription", XWW_DESCRIPTION
            VALUE "FileVersion", XWW_VERSION_TEXT
            VALUE "InternalName", XWW_FILENAME
            VALUE "LegalCopyright", "Copyright (c) 2026 xwwraw"
            VALUE "OriginalFilename", XWW_FILENAME
            VALUE "ProductName", "xwwPulse"
            VALUE "ProductVersion", XWW_VERSION_TEXT
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
