#pragma once
#include "core.h"
namespace mc::task {
bool PromptCommand(HWND owner, std::wstring& command);
void ShowDetails(HWND owner, const std::wstring& title, const std::wstring& text);
bool ChooseAffinity(HWND owner, DWORD_PTR current, DWORD_PTR allowed, DWORD_PTR& chosen);
} // namespace mc::task
