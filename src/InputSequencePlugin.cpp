#include <windows.h>
#include <algorithm>
#include <commctrl.h>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <string>
#include <vector>

// Minimal Notepad++ and Scintilla API declarations.  Keeping these local makes
// the project buildable without copying the Notepad++ source tree.
constexpr UINT NPPMSG = WM_USER + 1000;
constexpr UINT NPPM_GETCURRENTSCINTILLA = NPPMSG + 4;
constexpr UINT SCI_GETSELECTIONS = 2570;
constexpr UINT SCI_GETSELECTIONNSTART = 2585;
constexpr UINT SCI_GETSELECTIONNEND = 2587;
constexpr UINT SCI_SETSEL = 2160;
constexpr UINT SCI_REPLACESEL = 2170;
constexpr UINT SCI_GETTEXT = 2182;
constexpr UINT SCI_GETTEXTLENGTH = 2183;
constexpr UINT SCI_SETTEXT = 2181;
constexpr UINT SCI_BEGINUNDOACTION = 2078;
constexpr UINT SCI_ENDUNDOACTION = 2079;
constexpr UINT SCI_UNDO = 2176;
constexpr UINT SCI_POINTXFROMPOSITION = 2164;
constexpr UINT SCI_POINTYFROMPOSITION = 2165;
constexpr UINT SCI_TEXTHEIGHT = 2279;
constexpr UINT SCI_LINEFROMPOSITION = 2166;
constexpr UINT SCI_POSITIONFROMLINE = 2167;

struct NppData { HWND nppHandle, scintillaMainHandle, scintillaSecondHandle; };
struct ShortcutKey { bool isCtrl, isAlt, isShift; unsigned char key; };
using PluginCommand = void (*)();
struct FuncItem {
    wchar_t itemName[64]; PluginCommand command; int commandId; bool initToCheck; ShortcutKey* shortcut;
};

namespace {
HINSTANCE g_instance{};
NppData g_npp{};
ShortcutKey g_shortcut{true, true, false, '0'};
FuncItem g_commands[2]{};
constexpr wchar_t DialogClass[] = L"InputSequenceDialogWindow";
constexpr int EditId = 1001;
constexpr int OkId = IDOK;
constexpr int CancelId = IDCANCEL;

struct SequenceSpec {
    long long start;
    long long step;
    int digits;
    int radix;
    bool alphabetic;
    std::string prefix;
    std::string suffix;
    bool literalOnly;
};
struct Selection { LRESULT start; LRESULT end; };
struct PreviewSession {
    HWND editor{};
    std::vector<Selection> ranges;
    std::string originalText;
    bool undoActionOpen{};
    bool previewApplied{};
};
PreviewSession g_preview;

void trim(std::wstring& text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) { text.clear(); return; }
    const auto last = text.find_last_not_of(L" \t\r\n");
    text = text.substr(first, last - first + 1);
}

bool parseInteger(const std::wstring& source, int radix, long long& value) {
    if (source.empty() || radix < 2 || radix > 36) return false;
    size_t index = 0;
    bool negative = false;
    if (source[index] == L'+' || source[index] == L'-') negative = source[index++] == L'-';
    if (index == source.size()) return false;
    unsigned long long total = 0;
    for (; index < source.size(); ++index) {
        wchar_t c = source[index];
        int digit = c >= L'0' && c <= L'9' ? c - L'0' :
                    c >= L'a' && c <= L'z' ? c - L'a' + 10 :
                    c >= L'A' && c <= L'Z' ? c - L'A' + 10 : -1;
        if (digit < 0 || digit >= radix || total > (ULLONG_MAX - digit) / static_cast<unsigned>(radix)) return false;
        total = total * radix + digit;
    }
    if (negative) {
        if (total > static_cast<unsigned long long>(LLONG_MAX) + 1ULL) return false;
        value = total == static_cast<unsigned long long>(LLONG_MAX) + 1ULL ? LLONG_MIN : -static_cast<long long>(total);
    } else {
        if (total > static_cast<unsigned long long>(LLONG_MAX)) return false;
        value = static_cast<long long>(total);
    }
    return true;
}

bool parseAlphabetic(const std::wstring& source, int radix, long long& value) {
    if (source.empty() || radix < 2 || radix > 26) return false;
    unsigned long long total = 0;
    for (wchar_t c : source) {
        int digit = c >= L'A' && c <= L'Z' ? c - L'A' : c >= L'a' && c <= L'z' ? c - L'a' : -1;
        if (digit < 0 || digit >= radix || total > (ULLONG_MAX - digit) / static_cast<unsigned>(radix)) return false;
        total = total * radix + digit;
    }
    if (total > static_cast<unsigned long long>(LLONG_MAX)) return false;
    value = static_cast<long long>(total);
    return true;
}

bool isAlphabetic(const std::wstring& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); });
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring removeLiteralQuotes(const std::wstring& text) {
    std::wstring result;
    bool quoted = false;
    for (wchar_t c : text) {
        if (c == L'\'') { quoted = !quoted; continue; }
        result += c;
    }
    return result;
}

bool parseDecimalPrefix(const std::wstring& source, long long& value, std::wstring& suffix) {
    size_t end = 0;
    while (end < source.size() && source[end] >= L'0' && source[end] <= L'9') ++end;
    if (end == 0) return false;
    suffix = source.substr(end);
    return parseInteger(source.substr(0, end), 10, value);
}

bool isSyntaxCharacter(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || c == L'+' || c == L'-' || c == L':' || iswspace(c);
}

void splitSequenceAndLiterals(const std::wstring& input, std::wstring& core, std::wstring& prefix, std::wstring& suffix) {
    bool quoted = false;
    bool coreStarted = false;
    bool suffixStarted = false;
    for (wchar_t c : input) {
        if (c == L'\'') { quoted = !quoted; continue; }
        if (!coreStarted && !quoted && ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))) {
            coreStarted = true;
        }
        if (coreStarted && !suffixStarted && !quoted && isSyntaxCharacter(c)) {
            core += c;
        } else if (!coreStarted) {
            prefix += c;
        } else {
            suffixStarted = true;
            suffix += c;
        }
    }
}

bool parseSpec(std::wstring input, SequenceSpec& result, std::wstring& error) {
    trim(input);
    std::wstring core, prefix, literalSuffix;
    splitSequenceAndLiterals(input, core, prefix, literalSuffix);
    trim(core);
    if (core.empty()) {
        result = {0, 0, 0, 10, false, toUtf8(prefix + literalSuffix), {}, true};
        return true;
    }
    input = core;
    std::vector<std::wstring> parts;
    size_t offset = 0;
    while (true) {
        const size_t colon = input.find(L':', offset);
        parts.push_back(input.substr(offset, colon == std::wstring::npos ? std::wstring::npos : colon - offset));
        if (colon == std::wstring::npos) break;
        offset = colon + 1;
    }
    if (parts.size() > 3) {
        error = L"Expected: start+step[suffix] [:digits [:radix]]"; return false;
    }
    for (auto& part : parts) trim(part);
    std::wstring expression = parts[0];
    long long digitsValue = 0, radixValue = 0;
    std::wstring suffixFromFields;
    bool radixSpecified = parts.size() == 3 && !parts[2].empty();
    const bool digitsSpecified = parts.size() >= 2 && !parts[1].empty();
    if (digitsSpecified) {
        std::wstring tail;
        if (!parseDecimalPrefix(parts[1], digitsValue, tail) || digitsValue < 0 || digitsValue > 128) { error = L"digits must be an integer between 0 and 128."; return false; }
        suffixFromFields += tail;
    }
    if (radixSpecified) {
        std::wstring tail;
        if (!parseDecimalPrefix(parts[2], radixValue, tail) || radixValue < 2 || radixValue > 36) { error = L"radix must be an integer between 2 and 36."; return false; }
        suffixFromFields += tail;
    }
    const size_t candidateOperator = expression.find_first_of(L"+-", 1);
    std::wstring start = candidateOperator == std::wstring::npos ? expression : expression.substr(0, candidateOperator);
    trim(start);
    const bool alphabetic = !radixSpecified && isAlphabetic(start);
    if (!radixSpecified) radixValue = alphabetic ? 26 : 10;
    if (!digitsSpecified && !alphabetic && start.size() > 1 && start[0] == L'0') digitsValue = static_cast<long long>(start.size());
    std::wstring step = L"1", suffix;
    bool subtract = false;
    bool hasOperator = false;
    if (candidateOperator != std::wstring::npos) {
        size_t stepStart = candidateOperator + 1;
        while (stepStart < expression.size() && iswspace(expression[stepStart])) ++stepStart;
        const wchar_t firstStepCharacter = stepStart < expression.size() ? expression[stepStart] : L'\0';
        const int firstStepDigit = firstStepCharacter >= L'0' && firstStepCharacter <= L'9' ? firstStepCharacter - L'0' : firstStepCharacter >= L'a' && firstStepCharacter <= L'z' ? firstStepCharacter - L'a' + 10 : firstStepCharacter >= L'A' && firstStepCharacter <= L'Z' ? firstStepCharacter - L'A' + 10 : -1;
        hasOperator = firstStepDigit >= 0 && firstStepDigit < radixValue;
        if (!hasOperator) {
            suffix = expression.substr(candidateOperator);
        } else {
        subtract = expression[candidateOperator] == L'-';
        size_t stepEnd = stepStart;
        while (stepEnd < expression.size()) {
            const wchar_t c = expression[stepEnd];
            const int digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'z' ? c - L'a' + 10 : c >= L'A' && c <= L'Z' ? c - L'A' + 10 : -1;
            if (digit < 0 || digit >= radixValue) break;
            ++stepEnd;
        }
        if (stepEnd > stepStart) step = expression.substr(stepStart, stepEnd - stepStart);
        suffix = expression.substr(stepEnd);
        }
    }
    trim(start); trim(step);
    long long startValue{}, stepValue{};
    const bool validStart = alphabetic ? parseAlphabetic(start, static_cast<int>(radixValue), startValue) : parseInteger(start, static_cast<int>(radixValue), startValue);
    if (!validStart || !parseInteger(step, static_cast<int>(radixValue), stepValue) || stepValue < 0) {
        error = L"start and step must be valid for the selected radix; step cannot be negative."; return false;
    }
    result = {startValue, subtract ? -stepValue : stepValue, static_cast<int>(digitsValue), static_cast<int>(radixValue), alphabetic, toUtf8(prefix), toUtf8(removeLiteralQuotes(suffix + suffixFromFields) + literalSuffix), false};
    return true;
}

std::string formatNumber(long long value, const SequenceSpec& spec) {
    const char* alphabet = spec.alphabetic ? "ABCDEFGHIJKLMNOPQRSTUVWXYZ" : "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    bool negative = value < 0;
    unsigned long long number = negative ? static_cast<unsigned long long>(-(value + 1)) + 1ULL : static_cast<unsigned long long>(value);
    std::string output;
    do { output.insert(output.begin(), alphabet[number % spec.radix]); number /= spec.radix; } while (number);
    while (static_cast<int>(output.size()) < spec.digits) output.insert(output.begin(), spec.alphabetic ? 'A' : '0');
    return spec.prefix + (negative ? "-" : "") + output + spec.suffix;
}

HWND currentScintilla() {
    int view = 0;
    SendMessage(g_npp.nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&view));
    return view == 0 ? g_npp.scintillaMainHandle : g_npp.scintillaSecondHandle;
}

void applySequence(HWND editor, const std::vector<Selection>& ranges, const SequenceSpec& spec) {
    if (ranges.empty()) return;
    std::vector<std::string> values;
    values.reserve(ranges.size());
    for (size_t i = 0; i < ranges.size(); ++i) {
        values.push_back(spec.literalOnly ? spec.suffix : formatNumber(spec.start + spec.step * static_cast<long long>(i), spec));
    }
    // SCI_SETSEL removes multi-selections, so positions must be captured above.
    // Work backwards: replacing later ranges cannot change earlier offsets.
    for (int i = static_cast<int>(ranges.size()) - 1; i >= 0; --i) {
        const Selection& range = ranges[static_cast<size_t>(i)];
        SendMessage(editor, SCI_SETSEL, range.start, range.end);
        const std::string& value = values[static_cast<size_t>(i)];
        SendMessage(editor, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>(value.c_str()));
    }
}

void startPreviewSession() {
    g_preview = {};
    g_preview.editor = currentScintilla();
    const int selectionCount = static_cast<int>(SendMessage(g_preview.editor, SCI_GETSELECTIONS, 0, 0));
    for (int i = 0; i < selectionCount; ++i) {
        g_preview.ranges.push_back({
            SendMessage(g_preview.editor, SCI_GETSELECTIONNSTART, i, 0),
            SendMessage(g_preview.editor, SCI_GETSELECTIONNEND, i, 0)
        });
    }
    const auto length = static_cast<size_t>(SendMessage(g_preview.editor, SCI_GETTEXTLENGTH, 0, 0));
    g_preview.originalText.resize(length + 1);
    SendMessage(g_preview.editor, SCI_GETTEXT, static_cast<WPARAM>(length + 1), reinterpret_cast<LPARAM>(g_preview.originalText.data()));
    g_preview.originalText.resize(length);
    SendMessage(g_preview.editor, SCI_BEGINUNDOACTION, 0, 0);
    g_preview.undoActionOpen = true;
}

void renderPreview(HWND dialog) {
    if (!g_preview.undoActionOpen) return;
    SendMessage(g_preview.editor, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(g_preview.originalText.c_str()));
    wchar_t buffer[512]{};
    GetWindowTextW(GetDlgItem(dialog, EditId), buffer, 512);
    SequenceSpec spec{}; std::wstring error;
    if (parseSpec(buffer, spec, error)) applySequence(g_preview.editor, g_preview.ranges, spec);
    g_preview.previewApplied = true;
}

void finishPreview(bool keepPreview) {
    if (!g_preview.undoActionOpen) return;
    if (!keepPreview && g_preview.previewApplied) {
        SendMessage(g_preview.editor, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(g_preview.originalText.c_str()));
    }
    SendMessage(g_preview.editor, SCI_ENDUNDOACTION, 0, 0);
    g_preview.undoActionOpen = false;
    if (!keepPreview && g_preview.previewApplied) SendMessage(g_preview.editor, SCI_UNDO, 0, 0);
}

POINT dialogPosition() {
    constexpr int width = 478, height = 145;
    if (g_preview.ranges.empty()) return {CW_USEDEFAULT, CW_USEDEFAULT};
    int lastLine = 0;
    for (const Selection& range : g_preview.ranges) {
        const int startLine = static_cast<int>(SendMessage(g_preview.editor, SCI_LINEFROMPOSITION, static_cast<WPARAM>(range.start), 0));
        const int endLine = static_cast<int>(SendMessage(g_preview.editor, SCI_LINEFROMPOSITION, static_cast<WPARAM>(range.end), 0));
        lastLine = (std::max)(lastLine, (std::max)(startLine, endLine));
    }
    const LRESULT selectionStart = SendMessage(g_preview.editor, SCI_POSITIONFROMLINE, static_cast<WPARAM>(lastLine), 0);
    POINT point{
        static_cast<LONG>(SendMessage(g_preview.editor, SCI_POINTXFROMPOSITION, 0, selectionStart)),
        static_cast<LONG>(SendMessage(g_preview.editor, SCI_POINTYFROMPOSITION, 0, selectionStart))
    };
    ClientToScreen(g_preview.editor, &point);
    const int lineHeight = static_cast<int>(SendMessage(g_preview.editor, SCI_TEXTHEIGHT, 0, 0));
    point.y += lineHeight;
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &monitor);
    if (point.y + height > monitor.rcWork.bottom) point.y -= height + lineHeight;
    point.x = (std::max)(monitor.rcWork.left, (std::min)(point.x, monitor.rcWork.right - width));
    point.y = (std::max)(monitor.rcWork.top, (std::min)(point.y, monitor.rcWork.bottom - height));
    return point;
}

LRESULT CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"輸入格式：<start> <operator> <step> : <digit> : <radix>", WS_CHILD | WS_VISIBLE, 16, 14, 442, 20, window, nullptr, g_instance, nullptr);
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0+1", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 16, 40, 442, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(EditId)), g_instance, nullptr);
        SendMessage(edit, EM_SETSEL, 0, -1);
        CreateWindowW(L"BUTTON", L"套用", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 290, 78, 80, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(OkId)), g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 378, 78, 80, 26, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(CancelId)), g_instance, nullptr);
        SetFocus(edit); renderPreview(window); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == EditId && HIWORD(wParam) == EN_CHANGE) { renderPreview(window); return 0; }
        if (LOWORD(wParam) == OkId) {
            wchar_t buffer[512]{}; GetWindowTextW(GetDlgItem(window, EditId), buffer, 512);
            SequenceSpec spec{}; std::wstring error;
            if (!parseSpec(buffer, spec, error)) { MessageBoxW(window, error.c_str(), L"Input Sequence", MB_OK | MB_ICONWARNING); return 0; }
            renderPreview(window); finishPreview(true); DestroyWindow(window); return 0;
        }
        if (LOWORD(wParam) == CancelId) { finishPreview(false); DestroyWindow(window); return 0; }
        break;
    case WM_CLOSE: finishPreview(false); DestroyWindow(window); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void openInputDialog() {
    WNDCLASSW cls{}; cls.hInstance = g_instance; cls.lpszClassName = DialogClass; cls.lpfnWndProc = dialogProc; cls.hCursor = LoadCursor(nullptr, IDC_ARROW); cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassW(&cls);
    startPreviewSession();
    const POINT position = dialogPosition();
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, DialogClass, L"Input Sequence", WS_CAPTION | WS_SYSMENU | WS_POPUP, position.x, position.y, 478, 145, g_npp.nppHandle, nullptr, g_instance, nullptr);
    EnableWindow(g_npp.nppHandle, FALSE); ShowWindow(window, SW_SHOW); UpdateWindow(window);
    MSG message;
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    EnableWindow(g_npp.nppHandle, TRUE); SetForegroundWindow(g_npp.nppHandle);
}

void showAbout() {
    MessageBoxW(
        g_npp.nppHandle,
        L"Input Sequence\n\n"
        L"\u529f\u80fd\u8aaa\u660e\uff1a\n"
        L"- \u7522\u751f\u9023\u7e8c\u6578\u5b57\u8207\u82f1\u6587\u5b57\u6bcd\u3002\n"
        L"- \u652f\u63f4\u56fa\u5b9a\u524d\u7db4\u3001\u5c3e\u78bc\u3001\u4f4d\u6578\u53ca\u9032\u4f4d\u5236\u3002\n"
        L"- \u591a\u91cd\u9078\u53d6\u8207\u5373\u6642\u9810\u89bd\u3002\n"
        L"- \u5feb\u901f\u9375\uff1aCtrl+Alt+0\u3002\n\n"
        L"\u4f5c\u8005\uff1adasu88\n"
        L"\u96fb\u5b50\u90f5\u4ef6\uff1adasu88@gmail.com",
        L"\u95dc\u65bc Input Sequence",
        MB_OK | MB_ICONINFORMATION);
}
}

extern "C" __declspec(dllexport) void setInfo(NppData data) { g_npp = data; }
extern "C" __declspec(dllexport) const wchar_t* getName() { return L"Input Sequence"; }
extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) {
    if (!g_commands[0].command) {
        wcscpy_s(g_commands[0].itemName, L"輸入連續編號");
        g_commands[0].command = openInputDialog; g_commands[0].shortcut = &g_shortcut;
        wcscpy_s(g_commands[1].itemName, L"\u95dc\u65bc Input Sequence");
        g_commands[1].command = showAbout;
    }
    *count = 2; return g_commands;
}
extern "C" __declspec(dllexport) void beNotified(void*) {}
extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) g_instance = instance; return TRUE; }
