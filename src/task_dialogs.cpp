#include "task_dialogs.h"
#include <commctrl.h>
#include <commdlg.h>

namespace mc::task {
namespace {
struct TextState {
    std::wstring title, text;
    bool readOnly = false;
};
INT_PTR CALLBACK TextProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto state = reinterpret_cast<TextState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<TextState*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, state->title.c_str());
        SetDlgItemTextW(dialog, 2101, state->text.c_str());
        SendDlgItemMessageW(dialog, 2101, EM_SETLIMITTEXT, 32767, 0);
        if (state->readOnly)
            SendDlgItemMessageW(dialog, 2101, EM_SETREADONLY, TRUE, 0);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wp) == 2102) {
            wchar_t file[32768]{};
            OPENFILENAMEW open{};
            open.lStructSize = sizeof(open);
            open.hwndOwner = dialog;
            open.lpstrFile = file;
            open.nMaxFile = 32768;
            open.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件\0*.*\0\0";
            open.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&open))
                SetDlgItemTextW(dialog, 2101, (L"\"" + std::wstring(file) + L"\"").c_str());
            return TRUE;
        }
        if (LOWORD(wp) == IDOK && state) {
            int length = GetWindowTextLengthW(GetDlgItem(dialog, 2101));
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetDlgItemTextW(dialog, 2101, text.data(), length + 1);
            text.resize(length);
            if (!state->readOnly && text.find_first_not_of(L" \t\r\n") == std::wstring::npos)
                return TRUE;
            state->text = std::move(text);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}
struct AffinityState {
    DWORD_PTR current, allowed, chosen;
};
INT_PTR CALLBACK AffinityProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    auto state = reinterpret_cast<AffinityState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    HWND list = GetDlgItem(dialog, 2103);
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<AffinityState*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNW column{};
        column.mask = LVCF_WIDTH;
        column.cx = 300;
        ListView_InsertColumn(list, 0, &column);
        for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8; ++bit)
            if (state->allowed & (static_cast<DWORD_PTR>(1) << bit)) {
                auto name = L"CPU " + std::to_wstring(bit);
                LVITEMW item{};
                item.mask = LVIF_TEXT | LVIF_PARAM;
                item.iItem = ListView_GetItemCount(list);
                item.pszText = name.data();
                item.lParam = bit;
                int row = ListView_InsertItem(list, &item);
                ListView_SetCheckState(list, row, (state->current & (static_cast<DWORD_PTR>(1) << bit)) != 0);
            }
        return TRUE;
    }
    if (message == WM_COMMAND && state) {
        if (LOWORD(wp) == 2104 || LOWORD(wp) == 2105) {
            for (int i = 0; i < ListView_GetItemCount(list); ++i)
                ListView_SetCheckState(list, i, LOWORD(wp) == 2104);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            DWORD_PTR mask = 0;
            for (int i = 0; i < ListView_GetItemCount(list); ++i)
                if (ListView_GetCheckState(list, i)) {
                    LVITEMW item{};
                    item.mask = LVIF_PARAM;
                    item.iItem = i;
                    ListView_GetItem(list, &item);
                    mask |= static_cast<DWORD_PTR>(1) << item.lParam;
                }
            if (!mask) {
                MessageBoxW(dialog, L"请至少选择一个 CPU。", L"CPU 关联性", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            state->chosen = mask;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace
bool PromptCommand(HWND owner, std::wstring& command) {
    TextState state{L"运行新任务", command, false};
    auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(2000), owner, TextProc,
                                  reinterpret_cast<LPARAM>(&state));
    if (result != IDOK)
        return false;
    command = std::move(state.text);
    return true;
}
void ShowDetails(HWND owner, const std::wstring& title, const std::wstring& text) {
    TextState state{title, text, true};
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(2001), owner, TextProc,
                    reinterpret_cast<LPARAM>(&state));
}
bool ChooseAffinity(HWND owner, DWORD_PTR current, DWORD_PTR allowed, DWORD_PTR& chosen) {
    AffinityState state{current, allowed, current};
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(2002), owner, AffinityProc,
                        reinterpret_cast<LPARAM>(&state)) != IDOK)
        return false;
    chosen = state.chosen;
    return true;
}
} // namespace mc::task
