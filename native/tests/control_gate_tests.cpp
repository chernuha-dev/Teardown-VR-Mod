// SPDX-License-Identifier: MIT
#include <windows.h>
extern "C" __declspec(dllimport) DWORD WINAPI TdvrStartControl(void*);
int main() {
    return TdvrStartControl(nullptr) == static_cast<DWORD>(E_ACCESSDENIED) ? 0 : 1;
}
