#pragma once
#include <guiddef.h>

// {B8F3E2D1-9A4C-4F5E-B6D7-8C1A3E5F7D9B}
// CLSID for the LVT TAP COM object.
// Used by both lvt.exe (caller) and lvt_tap.dll (implementation).
static const CLSID CLSID_LvtTap =
    { 0xB8F3E2D1, 0x9A4C, 0x4F5E, { 0xB6, 0xD7, 0x8C, 0x1A, 0x3E, 0x5F, 0x7D, 0x9B } };
