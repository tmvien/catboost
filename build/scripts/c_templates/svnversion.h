#pragma once

// Permit compilation with NO_UTIL():
// util/system/compiler.h
#if !defined(Y_HIDDEN)
#if defined(__GNUC__)
#define Y_HIDDEN __attribute__((visibility("hidden")))
#else
#define Y_HIDDEN
#endif
#endif

#if defined(__cplusplus)
extern "C" {
#endif
const char* GetProgramSvnVersion() Y_HIDDEN; // verbose multiline message
void PrintProgramSvnVersion() Y_HIDDEN;
const char* GetArcadiaSourcePath() Y_HIDDEN; // "/home/myltsev/arcadia"
const char* GetArcadiaSourceUrl() Y_HIDDEN;  // "svn+ssh://arcadia.yandex.ru/arc/trunk/arcadia"
const char* GetArcadiaLastChange() Y_HIDDEN; // "2902074"
int GetArcadiaLastChangeNum() Y_HIDDEN; // 2902074
const char* GetArcadiaLastAuthor() Y_HIDDEN; // "dieash"
int   GetProgramSvnRevision() Y_HIDDEN;     // 2902074
const char* GetProgramHgHash() Y_HIDDEN;
void PrintSvnVersionAndExit(int argc, char* argv[]) Y_HIDDEN;
void PrintSvnVersionAndExitEx(int argc, char* argv[], const char* opts) Y_HIDDEN;
void PrintSvnVersionAndExit0() Y_HIDDEN;
const char* GetProgramScmData() Y_HIDDEN; // verbose multiline message
const char* GetProgramBuildUser() Y_HIDDEN;
const char* GetProgramBuildHost() Y_HIDDEN;
const char* GetProgramBuildDate() Y_HIDDEN;
const char* GetVCS() Y_HIDDEN;
const char* GetBranch() Y_HIDDEN;
#if defined(__cplusplus)
}
#endif