// Host TrueType files that stand in for the PSP firmware fonts (sceFont HLE).
#pragma once
#include <string>

// Resolves the regular face (PSPRECOMP_FONT, else DejaVu Sans) and its
// "<name>-Bold.ttf" sibling (empty when absent). Returns false if no regular
// face is readable. Text drawn by the game through sceFont uses exactly these
// files, so UI the runtime draws itself can match it.
bool psp_font_host_files(std::string* regular, std::string* bold);
