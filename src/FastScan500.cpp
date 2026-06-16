#include "VirtualDubToolAPI_min.h"
#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <new>
#include <string>
#include <vector>

#define FASTSCAN_PLUGIN_NAME_W L"Fast Scan 500 / 2000 / 3000 fps"
#define FASTSCAN_PLUGIN_DESCRIPTION_W L"Selectable 500, 2000, and 3000 fps timeline scan buttons after Mark Out. Enter or a click (not a drag) in the input/output video pane pauses or continues; timeline clicks stop."
#define FASTSCAN_COMMAND_ID_A "Tools.FastScan500"
#define FASTSCAN_TIMER_CLASS_W L"VirtualDub2.FastScan500.TimerWindow"

namespace {

constexpr int kDefaultFramesPerSecond = 500;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT_PTR kToolbarAttachTimerId = 2;
constexpr UINT kTimerIntervalMs = 5;
constexpr UINT kToolbarAttachIntervalMs = 250;
constexpr int kMarkInControlId = 513;
constexpr int kMarkOutControlId = 514;
constexpr int kInputPaneControlId = 1;
constexpr int kOutputPaneControlId = 2;
constexpr int kToolbarFontScalePercent = 72;
constexpr UINT_PTR kPositionControlSubclassId = 0xF500;
constexpr UINT_PTR kVideoPaneSubclassId = 0xF501;
constexpr wchar_t kPositionControlClassName[] = L"birdyPositionControl";
constexpr wchar_t kActiveSpeedProperty[] =
    L"VirtualDub2.FastScan.ActiveSpeed.85E26B37-83AB-45B7-A0C3-9A746FFAF0F0";

struct ToolbarButtonDefinition {
    int framesPerSecond;
    int controlId;
    const wchar_t* text;
};

constexpr ToolbarButtonDefinition kToolbarButtonDefinitions[] = {
    {500,  0x6F50, L"500"},
    {2000, 0x6F51, L"2000"},
    {3000, 0x6F52, L"3000"},
};

constexpr std::size_t kToolbarButtonCount =
    std::size(kToolbarButtonDefinitions);

HINSTANCE gModule = nullptr;

HWND ToHWND(VDXHWND hwnd) noexcept {
    return reinterpret_cast<HWND>(hwnd);
}

bool CopyText(char* destination, int destinationSize, const char* source) noexcept {
    if (!destination || destinationSize <= 0 || !source) {
        return false;
    }

    const std::size_t capacity = static_cast<std::size_t>(destinationSize);
    const std::size_t sourceLength = std::strlen(source);
    const std::size_t copyLength = (std::min)(sourceLength, capacity - 1);
    std::memcpy(destination, source, copyLength);
    destination[copyLength] = '\0';
    return true;
}

std::wstring ToLower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return text;
}

class FastScanController final {
public:
    FastScanController() noexcept {
        if (!QueryPerformanceFrequency(&performanceFrequency_)) {
            performanceFrequency_.QuadPart = 1;
        }
    }

    void Attach(VDXHWND hwndParent, IVDToolCallbacks* callbacks) {
        SetHost(hwndParent, callbacks);
        EnsureTimerWindow();
        InstallToolbarButtons();
        RefreshVideoPaneHooks();
        // Keep this timer running: VirtualDub2 may recreate its display child
        // windows when the renderer, pane layout, or source changes.
        StartToolbarAttachTimer();
    }

    void Shutdown() noexcept {
        Pause();
        ClearActiveSpeedIfOwned();
        StopToolbarAttachTimer();
        RemoveVideoPaneHooks();
        RemoveToolbarButtons();
        DestroyTimerWindow();
        callbacks_ = nullptr;
        mainWindow_ = nullptr;
        activated_ = false;
        lastAppliedPositionValid_ = false;
    }

    bool Execute(VDXHWND hwndParent, IVDToolCallbacks* callbacks) {
        return ExecuteSpeed(kDefaultFramesPerSecond, hwndParent, callbacks);
    }

    bool ExecuteSpeed(int framesPerSecond, VDXHWND hwndParent,
                      IVDToolCallbacks* callbacks) {
        SetHost(hwndParent, callbacks);
        activated_ = true;

        if (running_ && IsSpeedActive(framesPerSecond)) {
            Pause();
            return true;
        }

        if (running_) {
            Pause();
        }

        activeSpeedFps_ = framesPerSecond;
        SetActiveSpeed();
        StartOrContinue();
        UpdateToolbarButtonStates();
        return true;
    }

    bool TranslateMessage(MSG& msg) {
        if (!activated_ || !IsActiveSpeed()) {
            return false;
        }

        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) &&
            msg.wParam == VK_RETURN && IsMessageInMainWindow(msg.hwnd)) {
            const bool plainEnter =
                (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
                (GetKeyState(VK_MENU) & 0x8000) == 0;

            if (!plainEnter) {
                return false;
            }

            if ((msg.lParam & (1LL << 30)) == 0) {
                Toggle();
            }
            return true;
        }

        if (running_ && msg.message == WM_LBUTTONDOWN &&
            !IsToolbarButtonMessage(msg.hwnd) && IsLikelyTimelineClick(msg)) {
            Pause();
            return false;
        }

        return false;
    }

    void HandleFileOpen() noexcept {
        Pause();
        lastAppliedPositionValid_ = false;
    }

private:
    void SetHost(VDXHWND hwndParent, IVDToolCallbacks* callbacks) noexcept {
        if (callbacks) {
            callbacks_ = callbacks;
        }
        if (hwndParent) {
            mainWindow_ = ToHWND(hwndParent);
        }
    }

    bool IsSpeedActive(int framesPerSecond) const noexcept {
        if (!mainWindow_ || !IsWindow(mainWindow_)) {
            return false;
        }
        const HANDLE value = GetPropW(mainWindow_, kActiveSpeedProperty);
        return reinterpret_cast<INT_PTR>(value) ==
               static_cast<INT_PTR>(framesPerSecond);
    }

    bool IsActiveSpeed() const noexcept {
        return IsSpeedActive(activeSpeedFps_);
    }

    void SetActiveSpeed() noexcept {
        if (mainWindow_ && IsWindow(mainWindow_)) {
            SetPropW(mainWindow_, kActiveSpeedProperty,
                     reinterpret_cast<HANDLE>(
                         static_cast<INT_PTR>(activeSpeedFps_)));
        }
    }

    void ClearActiveSpeedIfOwned() noexcept {
        if (IsActiveSpeed()) {
            RemovePropW(mainWindow_, kActiveSpeedProperty);
        }
    }

    static LRESULT CALLBACK TimerWindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                            LPARAM lParam) {
        FastScanController* self = reinterpret_cast<FastScanController*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<FastScanController*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }

        if (self && message == WM_TIMER) {
            if (wParam == kTimerId) {
                self->OnTimer();
                return 0;
            }
            if (wParam == kToolbarAttachTimerId) {
                self->InstallToolbarButtons();
                self->RefreshVideoPaneHooks();
                return 0;
            }
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static bool RegisterTimerWindowClass() {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = gModule;
        windowClass.lpfnWndProc = &FastScanController::TimerWindowProc;
        windowClass.lpszClassName = FASTSCAN_TIMER_CLASS_W;

        if (RegisterClassExW(&windowClass)) {
            return true;
        }
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    bool EnsureTimerWindow() {
        if (timerWindow_) {
            return true;
        }
        if (!RegisterTimerWindowClass()) {
            return false;
        }

        timerWindow_ = CreateWindowExW(
            0, FASTSCAN_TIMER_CLASS_W, L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, gModule, this);
        return timerWindow_ != nullptr;
    }

    void DestroyTimerWindow() noexcept {
        if (timerWindow_) {
            DestroyWindow(timerWindow_);
            timerWindow_ = nullptr;
        }
    }

    void StartTimer() {
        if (EnsureTimerWindow()) {
            SetTimer(timerWindow_, kTimerId, kTimerIntervalMs, nullptr);
        }
    }

    void StopTimer() noexcept {
        if (timerWindow_) {
            KillTimer(timerWindow_, kTimerId);
        }
    }

    void StartToolbarAttachTimer() {
        if (EnsureTimerWindow()) {
            SetTimer(timerWindow_, kToolbarAttachTimerId,
                     kToolbarAttachIntervalMs, nullptr);
        }
    }

    void StopToolbarAttachTimer() noexcept {
        if (timerWindow_) {
            KillTimer(timerWindow_, kToolbarAttachTimerId);
        }
    }

    IVDTimeline* Timeline() const noexcept {
        return callbacks_ ? callbacks_->GetTimeline() : nullptr;
    }

    bool HasOpenFile() const {
        if (!callbacks_) {
            return false;
        }

        wchar_t fileName[2]{};
        return callbacks_->GetFileName(fileName, std::size(fileName)) > 0 ||
               fileName[0] != L'\0';
    }

    static bool FindLastFrame(IVDTimeline& timeline, int64& lastFrame) {
        const int subsetCount = timeline.GetSubsetCount();
        if (subsetCount <= 0) {
            return false;
        }

        int64 maximumEnd = 0;
        for (int index = 0; index < subsetCount; ++index) {
            int64 start = 0;
            int64 end = 0;
            timeline.GetSubsetRange(index, start, end);
            if (end > maximumEnd) {
                maximumEnd = end;
            }
        }

        if (maximumEnd <= 0) {
            return false;
        }

        lastFrame = maximumEnd - 1;
        return true;
    }

    void Toggle() {
        if (running_) {
            Pause();
        } else {
            StartOrContinue();
        }
    }

    void StartOrContinue() {
        IVDTimeline* timeline = Timeline();
        if (!timeline || !HasOpenFile()) {
            MessageBeep(MB_ICONWARNING);
            return;
        }

        int64 lastFrame = 0;
        if (!FindLastFrame(*timeline, lastFrame)) {
            MessageBeep(MB_ICONWARNING);
            return;
        }

        const int64 currentPosition = timeline->GetTimelinePos();
        if (currentPosition >= lastFrame) {
            MessageBeep(MB_OK);
            return;
        }

        basePosition_ = (std::max<int64>)(0, currentPosition);
        lastAppliedPosition_ = basePosition_;
        lastAppliedPositionValid_ = true;
        QueryPerformanceCounter(&startCounter_);
        running_ = true;
        StartTimer();
        UpdateToolbarButtonStates();
    }

    void Pause() noexcept {
        running_ = false;
        StopTimer();
        UpdateToolbarButtonStates();
    }

    void OnTimer() {
        if (!running_) {
            return;
        }

        // Another FastScan DLL was selected. Stop this instance immediately.
        if (!IsActiveSpeed()) {
            Pause();
            return;
        }

        IVDTimeline* timeline = Timeline();
        if (!timeline) {
            Pause();
            return;
        }

        const int64 observedPosition = timeline->GetTimelinePos();
        if (lastAppliedPositionValid_ && observedPosition != lastAppliedPosition_) {
            Pause();
            return;
        }

        int64 lastFrame = 0;
        if (!FindLastFrame(*timeline, lastFrame)) {
            Pause();
            return;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double elapsedSeconds =
            static_cast<double>(now.QuadPart - startCounter_.QuadPart) /
            static_cast<double>(performanceFrequency_.QuadPart);
        const long double advancedFrames =
            std::floor(static_cast<long double>(elapsedSeconds) *
                       static_cast<long double>(activeSpeedFps_));
        const long double requested =
            static_cast<long double>(basePosition_) + advancedFrames;

        int64 targetPosition = lastFrame;
        if (requested < static_cast<long double>(lastFrame)) {
            targetPosition = static_cast<int64>(requested);
        }

        targetPosition = (std::max<int64>)(0, targetPosition);
        if (!lastAppliedPositionValid_ || targetPosition != lastAppliedPosition_) {
            timeline->SetTimelinePos(targetPosition);
            lastAppliedPosition_ = targetPosition;
            lastAppliedPositionValid_ = true;
        }

        if (targetPosition >= lastFrame) {
            Pause();
        }
    }

    struct PositionControlSearchContext {
        HWND bestWindow = nullptr;
        LONG bestBottom = LONG_MIN;
        LONG bestWidth = 0;
    };

    static BOOL CALLBACK FindPositionControlProc(HWND hwnd, LPARAM lParam) {
        auto* context = reinterpret_cast<PositionControlSearchContext*>(lParam);
        if (!context) {
            return TRUE;
        }

        wchar_t className[128]{};
        if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) <= 0 ||
            std::wcscmp(className, kPositionControlClassName) != 0) {
            return TRUE;
        }

        HWND markOut = GetDlgItem(hwnd, kMarkOutControlId);
        if (!markOut || !IsWindowVisible(markOut)) {
            return TRUE;
        }

        RECT rect{};
        if (!GetWindowRect(hwnd, &rect)) {
            return TRUE;
        }

        const LONG width = rect.right - rect.left;
        if (!context->bestWindow || rect.bottom > context->bestBottom ||
            (rect.bottom == context->bestBottom && width > context->bestWidth)) {
            context->bestWindow = hwnd;
            context->bestBottom = rect.bottom;
            context->bestWidth = width;
        }
        return TRUE;
    }

    HWND FindMainPositionControl() const noexcept {
        if (!mainWindow_ || !IsWindow(mainWindow_)) {
            return nullptr;
        }

        PositionControlSearchContext context{};
        EnumChildWindows(mainWindow_, &FastScanController::FindPositionControlProc,
                         reinterpret_cast<LPARAM>(&context));
        return context.bestWindow;
    }

    static bool GetRectInParent(HWND child, HWND parent, RECT& rect) noexcept {
        if (!child || !parent || !IsWindow(child) || !IsWindow(parent) ||
            !GetWindowRect(child, &rect)) {
            return false;
        }

        MapWindowPoints(HWND_DESKTOP, parent,
                        reinterpret_cast<POINT*>(&rect), 2);
        return true;
    }

    struct StatusControlSearchContext {
        HWND parent = nullptr;
        HWND markIn = nullptr;
        HWND markOut = nullptr;
        std::array<HWND, kToolbarButtonCount> fastScanButtons{};
        RECT markOutRect{};
        HWND bestWindow = nullptr;
        LONG bestDistance = LONG_MAX;
        LONG bestWidth = 0;
        bool bestIsWide = false;
    };

    static BOOL CALLBACK FindStatusControlProc(HWND hwnd, LPARAM lParam) {
        auto* context = reinterpret_cast<StatusControlSearchContext*>(lParam);
        if (!context || GetParent(hwnd) != context->parent ||
            hwnd == context->markIn || hwnd == context->markOut ||
            !IsWindowVisible(hwnd)) {
            return TRUE;
        }

        for (HWND button : context->fastScanButtons) {
            if (hwnd == button) {
                return TRUE;
            }
        }

        RECT rect{};
        if (!GetRectInParent(hwnd, context->parent, rect)) {
            return TRUE;
        }

        const LONG width = rect.right - rect.left;
        const LONG height = rect.bottom - rect.top;
        const LONG markOutWidth =
            context->markOutRect.right - context->markOutRect.left;
        const LONG markOutHeight =
            context->markOutRect.bottom - context->markOutRect.top;
        const LONG overlapTop = (std::max)(rect.top, context->markOutRect.top);
        const LONG overlapBottom = (std::min)(rect.bottom, context->markOutRect.bottom);
        const LONG verticalOverlap = overlapBottom - overlapTop;
        const LONG distance = rect.left - context->markOutRect.right;

        if (width <= 0 || height <= 0 || verticalOverlap <= 0 || distance < -2) {
            return TRUE;
        }

        // The status field is normally the first broad control after Mark Out.
        // Prefer a control at least twice as wide as a toolbar button, then the
        // nearest control if a particular VirtualDub2 build uses a narrow field.
        const bool isWide = markOutWidth > 0 && width >= markOutWidth * 2;
        const bool better =
            !context->bestWindow ||
            (isWide && !context->bestIsWide) ||
            (isWide == context->bestIsWide && distance < context->bestDistance) ||
            (isWide == context->bestIsWide && distance == context->bestDistance &&
             width > context->bestWidth);

        if (better && (markOutHeight <= 0 || verticalOverlap * 2 >= markOutHeight)) {
            context->bestWindow = hwnd;
            context->bestDistance = distance;
            context->bestWidth = width;
            context->bestIsWide = isWide;
        }
        return TRUE;
    }

    bool EnsureStatusControl() noexcept {
        if (statusControl_ && IsWindow(statusControl_) &&
            GetParent(statusControl_) == positionControl_) {
            return true;
        }

        statusControl_ = nullptr;
        statusNativeGap_ = 0;
        statusNativeGapValid_ = false;

        if (!positionControl_ || !IsWindow(positionControl_)) {
            return false;
        }

        HWND markOut = GetDlgItem(positionControl_, kMarkOutControlId);
        if (!markOut) {
            return false;
        }

        RECT markOutRect{};
        if (!GetRectInParent(markOut, positionControl_, markOutRect)) {
            return false;
        }

        StatusControlSearchContext context{};
        context.parent = positionControl_;
        context.markIn = GetDlgItem(positionControl_, kMarkInControlId);
        context.markOut = markOut;
        context.fastScanButtons = toolbarButtons_;
        context.markOutRect = markOutRect;
        EnumChildWindows(positionControl_, &FastScanController::FindStatusControlProc,
                         reinterpret_cast<LPARAM>(&context));

        if (!context.bestWindow) {
            return false;
        }

        statusControl_ = context.bestWindow;
        statusNativeGap_ = static_cast<int>((std::max<LONG>)(0, context.bestDistance));
        statusNativeGapValid_ = true;
        return true;
    }

    void RestoreStatusControl() noexcept {
        if (!positionControl_ || !IsWindow(positionControl_) ||
            !statusControl_ || !IsWindow(statusControl_)) {
            return;
        }

        HWND markOut = GetDlgItem(positionControl_, kMarkOutControlId);
        RECT markOutRect{};
        RECT statusRect{};
        if (!markOut || !GetRectInParent(markOut, positionControl_, markOutRect) ||
            !GetRectInParent(statusControl_, positionControl_, statusRect)) {
            return;
        }

        const int nativeGap = statusNativeGapValid_ ? statusNativeGap_ : 0;
        const int x = static_cast<int>(markOutRect.right) + nativeGap;
        const int right = static_cast<int>(statusRect.right);
        const int width = (std::max)(1, right - x);
        const int height = (std::max)(1,
            static_cast<int>(statusRect.bottom - statusRect.top));

        SetWindowPos(statusControl_, nullptr, x, static_cast<int>(statusRect.top),
                     width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    }

    bool AreToolbarButtonsInstalled() const noexcept {
        if (!positionControl_ || !IsWindow(positionControl_)) {
            return false;
        }
        for (HWND button : toolbarButtons_) {
            if (!button || !IsWindow(button)) {
                return false;
            }
        }
        return true;
    }

    static LRESULT CALLBACK PositionControlSubclassProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData) {
        auto* self = reinterpret_cast<FastScanController*>(referenceData);
        if (!self) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
            const int controlId = static_cast<int>(LOWORD(wParam));
            const HWND clickedButton = reinterpret_cast<HWND>(lParam);
            for (std::size_t index = 0; index < kToolbarButtonCount; ++index) {
                if (controlId == kToolbarButtonDefinitions[index].controlId &&
                    clickedButton == self->toolbarButtons_[index]) {
                    self->ExecuteSpeed(
                        kToolbarButtonDefinitions[index].framesPerSecond,
                        reinterpret_cast<VDXHWND>(self->mainWindow_),
                        self->callbacks_);
                    return 0;
                }
            }
        }

        if (message == WM_NCDESTROY) {
            self->toolbarButtons_.fill(nullptr);
            self->statusControl_ = nullptr;
            self->statusNativeGap_ = 0;
            self->statusNativeGapValid_ = false;
            self->positionControl_ = nullptr;
            RemoveWindowSubclass(hwnd, &FastScanController::PositionControlSubclassProc,
                                 subclassId);
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (message == WM_SIZE || message == WM_WINDOWPOSCHANGED) {
            self->PositionToolbarButtons();
        }
        return result;
    }

    bool InstallToolbarButtons() {
        if (AreToolbarButtonsInstalled()) {
            PositionToolbarButtons();
            UpdateToolbarButtonStates();
            return true;
        }

        RemoveToolbarButtons();

        HWND positionControl = FindMainPositionControl();
        if (!positionControl) {
            return false;
        }

        std::array<HWND, kToolbarButtonCount> buttons{};
        for (std::size_t index = 0; index < kToolbarButtonCount; ++index) {
            const auto& definition = kToolbarButtonDefinitions[index];
            buttons[index] = CreateWindowExW(
                0, L"BUTTON", definition.text,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE,
                0, 0, 0, 0, positionControl,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(definition.controlId)),
                gModule, nullptr);
            if (!buttons[index]) {
                for (HWND button : buttons) {
                    if (button && IsWindow(button)) {
                        DestroyWindow(button);
                    }
                }
                return false;
            }
        }

        if (!SetWindowSubclass(positionControl,
                               &FastScanController::PositionControlSubclassProc,
                               kPositionControlSubclassId,
                               reinterpret_cast<DWORD_PTR>(this))) {
            for (HWND button : buttons) {
                DestroyWindow(button);
            }
            return false;
        }

        positionControl_ = positionControl;
        toolbarButtons_ = buttons;
        EnsureStatusControl();

        if (HWND markOut = GetDlgItem(positionControl_, kMarkOutControlId)) {
            ApplyToolbarButtonFont(markOut);
        }

        PositionToolbarButtons();
        UpdateToolbarButtonStates();
        return true;
    }

    void RemoveToolbarButtons() noexcept {
        RestoreStatusControl();

        if (positionControl_ && IsWindow(positionControl_)) {
            RemoveWindowSubclass(positionControl_,
                                 &FastScanController::PositionControlSubclassProc,
                                 kPositionControlSubclassId);
        }

        for (HWND button : toolbarButtons_) {
            if (button && IsWindow(button)) {
                DestroyWindow(button);
            }
        }
        if (toolbarButtonFont_) {
            DeleteObject(toolbarButtonFont_);
            toolbarButtonFont_ = nullptr;
        }

        toolbarButtons_.fill(nullptr);
        statusControl_ = nullptr;
        statusNativeGap_ = 0;
        statusNativeGapValid_ = false;
        positionControl_ = nullptr;
    }

    void ApplyToolbarButtonFont(HWND referenceButton) noexcept {
        if (!referenceButton || !IsWindow(referenceButton)) {
            return;
        }

        const HFONT referenceFont = reinterpret_cast<HFONT>(
            SendMessageW(referenceButton, WM_GETFONT, 0, 0));
        if (!referenceFont) {
            return;
        }

        LOGFONTW logFont{};
        if (GetObjectW(referenceFont, static_cast<int>(sizeof(logFont)),
                       &logFont) == static_cast<int>(sizeof(logFont))) {
            LONG scaledHeight =
                MulDiv(logFont.lfHeight, kToolbarFontScalePercent, 100);
            if (scaledHeight == 0) {
                scaledHeight = logFont.lfHeight < 0 ? -1 : 1;
            }
            logFont.lfHeight = scaledHeight;
            logFont.lfWidth = 0;
            toolbarButtonFont_ = CreateFontIndirectW(&logFont);
        }

        const HFONT buttonFont = toolbarButtonFont_ ? toolbarButtonFont_ : referenceFont;
        for (HWND button : toolbarButtons_) {
            if (button && IsWindow(button)) {
                SendMessageW(button, WM_SETFONT,
                             reinterpret_cast<WPARAM>(buttonFont), TRUE);
            }
        }
    }

    int MeasureToolbarButtonWidth(std::size_t index, int baseWidth,
                                  int buttonHeight) const noexcept {
        if (index >= kToolbarButtonCount ||
            !toolbarButtons_[index] || !IsWindow(toolbarButtons_[index])) {
            return baseWidth;
        }

        int minimumWidth = baseWidth;
        if (kToolbarButtonDefinitions[index].framesPerSecond >= 1000) {
            minimumWidth = (std::max)(minimumWidth, MulDiv(baseWidth, 135, 100));
        }

        HDC deviceContext = GetDC(toolbarButtons_[index]);
        if (!deviceContext) {
            return minimumWidth;
        }

        const HFONT font = reinterpret_cast<HFONT>(
            SendMessageW(toolbarButtons_[index], WM_GETFONT, 0, 0));
        const HGDIOBJ previousFont = font ? SelectObject(deviceContext, font) : nullptr;

        SIZE textSize{};
        const wchar_t* text = kToolbarButtonDefinitions[index].text;
        const int textLength = static_cast<int>(std::wcslen(text));
        const bool measured =
            GetTextExtentPoint32W(deviceContext, text, textLength, &textSize) != FALSE;

        if (previousFont) {
            SelectObject(deviceContext, previousFont);
        }
        ReleaseDC(toolbarButtons_[index], deviceContext);

        if (!measured) {
            return minimumWidth;
        }

        const int horizontalPadding = (std::max)(6, buttonHeight / 3);
        return (std::max)(minimumWidth,
                          static_cast<int>(textSize.cx) + horizontalPadding * 2);
    }

    void PositionToolbarButtons() noexcept {
        if (!AreToolbarButtonsInstalled()) {
            return;
        }

        HWND markOut = GetDlgItem(positionControl_, kMarkOutControlId);
        if (!markOut || !IsWindowVisible(markOut)) {
            for (HWND button : toolbarButtons_) {
                ShowWindow(button, SW_HIDE);
            }
            return;
        }

        RECT markOutRect{};
        if (!GetWindowRect(markOut, &markOutRect)) {
            return;
        }
        MapWindowPoints(HWND_DESKTOP, positionControl_,
                        reinterpret_cast<POINT*>(&markOutRect), 2);

        int gap = 2;
        if (HWND markIn = GetDlgItem(positionControl_, kMarkInControlId)) {
            RECT markInRect{};
            if (GetWindowRect(markIn, &markInRect)) {
                MapWindowPoints(HWND_DESKTOP, positionControl_,
                                reinterpret_cast<POINT*>(&markInRect), 2);
                const int measuredGap = markOutRect.left - markInRect.right;
                if (measuredGap >= 0 && measuredGap <= 32) {
                    gap = measuredGap;
                }
            }
        }

        const int baseWidth = (std::max)(
            1, static_cast<int>(markOutRect.right - markOutRect.left));
        const int height = (std::max)(
            1, static_cast<int>(markOutRect.bottom - markOutRect.top));
        const int firstX = markOutRect.right + gap;
        const int y = markOutRect.top;

        std::array<int, kToolbarButtonCount> widths{};
        int totalButtonWidth = 0;
        for (std::size_t index = 0; index < kToolbarButtonCount; ++index) {
            widths[index] = MeasureToolbarButtonWidth(index, baseWidth, height);
            totalButtonWidth += widths[index];
        }

        RECT statusRect{};
        const bool haveStatus = EnsureStatusControl() &&
            GetRectInParent(statusControl_, positionControl_, statusRect);
        const int statusGap = (std::max)(
            gap, statusNativeGapValid_ ? statusNativeGap_ : 0);

        if (haveStatus) {
            const int minimumStatusWidth = baseWidth * 3;
            const int availableForButtonWidths = (std::max)(
                0, static_cast<int>(statusRect.right) - firstX -
                   statusGap - minimumStatusWidth -
                   gap * static_cast<int>(kToolbarButtonCount - 1));
            int excess = totalButtonWidth - availableForButtonWidths;

            // Preserve the compact 500 button. Contract the two four-digit
            // buttons first; the smaller font still keeps their labels legible.
            for (std::size_t index = kToolbarButtonCount; index-- > 1 && excess > 0;) {
                const int reducible = (std::max)(0, widths[index] - baseWidth);
                const int reduction = (std::min)(reducible, excess);
                widths[index] -= reduction;
                excess -= reduction;
            }
        }

        int x = firstX;
        int lastButtonRight = x;
        for (std::size_t index = 0; index < kToolbarButtonCount; ++index) {
            SetWindowPos(toolbarButtons_[index], HWND_TOP, x, y,
                         widths[index], height,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            lastButtonRight = x + widths[index];
            x = lastButtonRight + gap;
        }

        // Move the status field after the complete button group while keeping
        // its original right edge. This remains idempotent across host resizes.
        if (haveStatus) {
            const int statusX = lastButtonRight + statusGap;
            const int statusRight = static_cast<int>(statusRect.right);
            const int statusWidth = (std::max)(1, statusRight - statusX);
            const int statusHeight = (std::max)(
                1, static_cast<int>(statusRect.bottom - statusRect.top));

            SetWindowPos(statusControl_, nullptr, statusX,
                         static_cast<int>(statusRect.top),
                         statusWidth, statusHeight,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }

        const BOOL enabled = IsWindowEnabled(markOut);
        for (HWND button : toolbarButtons_) {
            EnableWindow(button, enabled);
        }
    }

    void UpdateToolbarButtonStates() noexcept {
        for (std::size_t index = 0; index < kToolbarButtonCount; ++index) {
            const HWND button = toolbarButtons_[index];
            if (!button || !IsWindow(button)) {
                continue;
            }

            const bool active = running_ && IsSpeedActive(
                kToolbarButtonDefinitions[index].framesPerSecond);
            SendMessageW(button, BM_SETCHECK,
                         active ? BST_CHECKED : BST_UNCHECKED, 0);
        }
    }

    static POINT GetMouseScreenPoint(HWND hwnd, LPARAM lParam) noexcept {
        POINT point{
            GET_X_LPARAM(lParam),
            GET_Y_LPARAM(lParam)
        };
        ClientToScreen(hwnd, &point);
        return point;
    }

    HWND VideoPaneForWindow(HWND candidate) const noexcept {
        if (IsWindowOrDescendant(inputPane_, candidate)) {
            return inputPane_;
        }
        if (IsWindowOrDescendant(outputPane_, candidate)) {
            return outputPane_;
        }
        return nullptr;
    }

    void CancelVideoPaneGesture() noexcept {
        videoPaneGestureActive_ = false;
        videoPaneGestureDragged_ = false;
        videoPaneGesturePane_ = nullptr;
        videoPaneGestureWindow_ = nullptr;
        videoPaneGestureStart_ = POINT{};
    }

    void BeginVideoPaneGesture(HWND hwnd, LPARAM lParam) noexcept {
        CancelVideoPaneGesture();

        HWND pane = VideoPaneForWindow(hwnd);
        if (!pane) {
            return;
        }

        videoPaneGestureActive_ = true;
        videoPaneGesturePane_ = pane;
        videoPaneGestureWindow_ = hwnd;
        videoPaneGestureStart_ = GetMouseScreenPoint(hwnd, lParam);
    }

    void UpdateVideoPaneGesture(HWND hwnd, LPARAM lParam) noexcept {
        if (!videoPaneGestureActive_ || videoPaneGestureDragged_) {
            return;
        }

        const POINT point = GetMouseScreenPoint(hwnd, lParam);
        const int thresholdX = (std::max)(
            1, (GetSystemMetrics(SM_CXDRAG) + 1) / 2);
        const int thresholdY = (std::max)(
            1, (GetSystemMetrics(SM_CYDRAG) + 1) / 2);
        const int deltaX = point.x - videoPaneGestureStart_.x;
        const int deltaY = point.y - videoPaneGestureStart_.y;

        if (deltaX <= -thresholdX || deltaX >= thresholdX ||
            deltaY <= -thresholdY || deltaY >= thresholdY) {
            videoPaneGestureDragged_ = true;
        }
    }

    bool EndVideoPaneGesture(HWND hwnd, LPARAM lParam) noexcept {
        if (!videoPaneGestureActive_) {
            return false;
        }

        UpdateVideoPaneGesture(hwnd, lParam);

        const POINT releasePoint = GetMouseScreenPoint(hwnd, lParam);
        RECT paneRect{};
        const bool releasedInOriginPane =
            videoPaneGesturePane_ && IsWindow(videoPaneGesturePane_) &&
            GetWindowRect(videoPaneGesturePane_, &paneRect) &&
            PtInRect(&paneRect, releasePoint) != FALSE;
        const bool isClick =
            !videoPaneGestureDragged_ && releasedInOriginPane;

        CancelVideoPaneGesture();
        return isClick;
    }

    void CancelVideoPaneGestureForWindow(HWND hwnd) noexcept {
        if (videoPaneGestureActive_ &&
            (hwnd == videoPaneGestureWindow_ || hwnd == videoPaneGesturePane_)) {
            CancelVideoPaneGesture();
        }
    }

    static LRESULT CALLBACK VideoPaneSubclassProc(
        HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData) {
        auto* self = reinterpret_cast<FastScanController*>(referenceData);
        if (!self) {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        if (message == WM_NCDESTROY) {
            self->CancelVideoPaneGestureForWindow(hwnd);
            RemoveWindowSubclass(hwnd,
                                 &FastScanController::VideoPaneSubclassProc,
                                 subclassId);
            self->ForgetVideoPaneWindow(hwnd);
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        bool toggleAfterHost = false;
        switch (message) {
        case WM_LBUTTONDOWN:
            self->BeginVideoPaneGesture(hwnd, lParam);
            break;

        case WM_MOUSEMOVE:
            if ((wParam & MK_LBUTTON) != 0) {
                self->UpdateVideoPaneGesture(hwnd, lParam);
            }
            break;

        case WM_LBUTTONUP:
            toggleAfterHost = self->EndVideoPaneGesture(hwnd, lParam);
            break;

        case WM_CANCELMODE:
            self->CancelVideoPaneGesture();
            break;

        case WM_CAPTURECHANGED:
            if (!self->IsCurrentVideoPaneWindow(
                    reinterpret_cast<HWND>(lParam))) {
                self->CancelVideoPaneGesture();
            }
            break;

        default:
            break;
        }

        // Let VirtualDub2 process the gesture first. This preserves normal pane
        // focus/zoom/drag behaviour and prevents a host-side position update
        // from immediately cancelling a just-started scan. Only a completed
        // press/release that stayed within the system drag threshold toggles.
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);

        if (toggleAfterHost) {
            self->HandleVideoPaneClick(hwnd);
        }

        return result;
    }

    void HandleVideoPaneClick(HWND clickedWindow) {
        if (!IsCurrentVideoPaneWindow(clickedWindow)) {
            return;
        }

        // A pane click toggles the most recently selected speed. Before any
        // speed button or Tools command has been used, the default is 500 fps.
        // If another FastScan DLL owns the shared property, claiming it makes
        // that instance stop on its next timer tick.
        activated_ = true;
        if (!IsActiveSpeed()) {
            SetActiveSpeed();
            StartOrContinue();
            UpdateToolbarButtonStates();
            return;
        }

        Toggle();
    }

    bool IsCurrentVideoPaneWindow(HWND candidate) const noexcept {
        return VideoPaneForWindow(candidate) != nullptr;
    }

    void ForgetVideoPaneWindow(HWND hwnd) noexcept {
        const auto it = std::remove(videoPaneWindows_.begin(),
                                    videoPaneWindows_.end(), hwnd);
        videoPaneWindows_.erase(it, videoPaneWindows_.end());
    }

    bool HookVideoPaneWindow(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) {
            return false;
        }

        if (std::find(videoPaneWindows_.begin(), videoPaneWindows_.end(), hwnd) !=
            videoPaneWindows_.end()) {
            return true;
        }

        if (!SetWindowSubclass(hwnd,
                               &FastScanController::VideoPaneSubclassProc,
                               kVideoPaneSubclassId,
                               reinterpret_cast<DWORD_PTR>(this))) {
            return false;
        }

        videoPaneWindows_.push_back(hwnd);
        return true;
    }

    static BOOL CALLBACK HookVideoPaneDescendantProc(HWND hwnd, LPARAM lParam) {
        auto* self = reinterpret_cast<FastScanController*>(lParam);
        if (self) {
            self->HookVideoPaneWindow(hwnd);
        }
        return TRUE;
    }

    void RefreshVideoPaneHooks() {
        if (!mainWindow_ || !IsWindow(mainWindow_)) {
            RemoveVideoPaneHooks();
            return;
        }

        inputPane_ = GetDlgItem(mainWindow_, kInputPaneControlId);
        outputPane_ = GetDlgItem(mainWindow_, kOutputPaneControlId);

        // Remove hooks from stale renderer windows or panes that were replaced.
        for (auto it = videoPaneWindows_.begin(); it != videoPaneWindows_.end();) {
            const HWND hwnd = *it;
            const bool valid = IsWindow(hwnd) && IsCurrentVideoPaneWindow(hwnd);
            if (!valid) {
                if (IsWindow(hwnd)) {
                    RemoveWindowSubclass(
                        hwnd, &FastScanController::VideoPaneSubclassProc,
                        kVideoPaneSubclassId);
                }
                it = videoPaneWindows_.erase(it);
            } else {
                ++it;
            }
        }

        const HWND panes[] = {inputPane_, outputPane_};
        for (HWND pane : panes) {
            if (!pane || !IsWindow(pane)) {
                continue;
            }

            HookVideoPaneWindow(pane);
            // EnumChildWindows enumerates all descendants, including the
            // renderer/display HWND that actually receives mouse clicks.
            EnumChildWindows(pane,
                             &FastScanController::HookVideoPaneDescendantProc,
                             reinterpret_cast<LPARAM>(this));
        }
    }

    void RemoveVideoPaneHooks() noexcept {
        CancelVideoPaneGesture();
        for (HWND hwnd : videoPaneWindows_) {
            if (hwnd && IsWindow(hwnd)) {
                RemoveWindowSubclass(hwnd,
                                     &FastScanController::VideoPaneSubclassProc,
                                     kVideoPaneSubclassId);
            }
        }
        videoPaneWindows_.clear();
        inputPane_ = nullptr;
        outputPane_ = nullptr;
    }

    bool IsToolbarButtonMessage(HWND messageWindow) const noexcept {
        return messageWindow &&
               std::find(toolbarButtons_.begin(), toolbarButtons_.end(),
                         messageWindow) != toolbarButtons_.end();
    }

    bool IsMessageInMainWindow(HWND messageWindow) const noexcept {
        if (!mainWindow_ || !messageWindow) {
            return false;
        }
        return messageWindow == mainWindow_ ||
               IsChild(mainWindow_, messageWindow) != FALSE;
    }

    static bool IsWindowOrDescendant(HWND parent, HWND candidate) noexcept {
        return parent && candidate &&
               (candidate == parent || IsChild(parent, candidate) != FALSE);
    }

    bool IsLikelyTimelineClick(const MSG& msg) const {
        if (!mainWindow_ || !IsWindow(mainWindow_)) {
            return false;
        }

        POINT point = msg.pt;
        if (point.x == 0 && point.y == 0 && msg.hwnd) {
            point.x = GET_X_LPARAM(msg.lParam);
            point.y = GET_Y_LPARAM(msg.lParam);
            ClientToScreen(msg.hwnd, &point);
        }

        RECT mainRect{};
        if (!GetWindowRect(mainWindow_, &mainRect) || !PtInRect(&mainRect, point)) {
            return false;
        }

        HWND hitWindow = WindowFromPoint(point);
        if (hitWindow && (hitWindow == mainWindow_ || IsChild(mainWindow_, hitWindow))) {
            // Pane gestures have their own click-vs-drag handling. Never let
            // the broad lower-window timeline heuristic classify them as
            // timeline clicks.
            if (IsCurrentVideoPaneWindow(hitWindow)) {
                return false;
            }

            wchar_t className[128]{};
            GetClassNameW(hitWindow, className, static_cast<int>(std::size(className)));
            const std::wstring lowered = ToLower(className);

            if (lowered.find(L"timeline") != std::wstring::npos ||
                lowered.find(L"position") != std::wstring::npos ||
                lowered.find(L"trackbar") != std::wstring::npos ||
                lowered.find(L"seek") != std::wstring::npos ||
                lowered == L"msctls_trackbar32") {
                return true;
            }

            RECT hitRect{};
            if (GetWindowRect(hitWindow, &hitRect)) {
                const int mainWidth = mainRect.right - mainRect.left;
                const int mainHeight = mainRect.bottom - mainRect.top;
                const int hitWidth = hitRect.right - hitRect.left;
                const int hitHeight = hitRect.bottom - hitRect.top;

                if (mainWidth > 0 && mainHeight > 0 &&
                    hitWidth * 2 >= mainWidth &&
                    hitHeight <= 220 &&
                    hitRect.top >= mainRect.top + mainHeight / 2) {
                    return true;
                }
            }
        }

        POINT clientPoint = point;
        ScreenToClient(mainWindow_, &clientPoint);
        RECT clientRect{};
        GetClientRect(mainWindow_, &clientRect);

        UINT dpi = 96;
        using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            auto getDpiForWindow = reinterpret_cast<GetDpiForWindowProc>(
                GetProcAddress(user32, "GetDpiForWindow"));
            if (getDpiForWindow) {
                dpi = getDpiForWindow(mainWindow_);
            }
        }

        const int lowerBand = MulDiv(180, static_cast<int>(dpi), 96);
        return clientPoint.y >= clientRect.bottom - lowerBand;
    }

private:
    IVDToolCallbacks* callbacks_ = nullptr;
    HWND mainWindow_ = nullptr;
    HWND timerWindow_ = nullptr;
    HWND positionControl_ = nullptr;
    std::array<HWND, kToolbarButtonCount> toolbarButtons_{};
    HFONT toolbarButtonFont_ = nullptr;
    HWND inputPane_ = nullptr;
    HWND outputPane_ = nullptr;
    std::vector<HWND> videoPaneWindows_;
    HWND videoPaneGesturePane_ = nullptr;
    HWND videoPaneGestureWindow_ = nullptr;
    POINT videoPaneGestureStart_{};
    bool videoPaneGestureActive_ = false;
    bool videoPaneGestureDragged_ = false;
    HWND statusControl_ = nullptr;
    int statusNativeGap_ = 0;
    bool statusNativeGapValid_ = false;
    bool activated_ = false;
    bool running_ = false;
    int activeSpeedFps_ = kDefaultFramesPerSecond;
    bool lastAppliedPositionValid_ = false;
    int64 basePosition_ = 0;
    int64 lastAppliedPosition_ = 0;
    LARGE_INTEGER performanceFrequency_{};
    LARGE_INTEGER startCounter_{};
};

FastScanController gController;

class FastScanTool final : public IVDXTool {
public:
    explicit FastScanTool(const VDXToolContext* context) noexcept
        : callbacks_(context ? context->mpCallbacks : nullptr) {
    }

    ~FastScanTool() {
        gController.Shutdown();
    }

    int VDXAPIENTRY AddRef() override {
        return static_cast<int>(++referenceCount_);
    }

    int VDXAPIENTRY Release() override {
        const long count = --referenceCount_;
        if (count == 0) {
            delete this;
            return 0;
        }
        return static_cast<int>(count);
    }

    void* VDXAPIENTRY AsInterface(uint32 iid) override {
        if (iid == IVDXTool::kIID || iid == IVDXUnknown::kIID) {
            return static_cast<IVDXTool*>(this);
        }
        return nullptr;
    }

    bool VDXAPIENTRY GetMenuInfo(int, char*, int, bool*) override {
        return false;
    }

    bool VDXAPIENTRY GetCommandId(int id, char* name, int nameSize) override {
        if (id != 0) {
            return false;
        }
        return CopyText(name, nameSize, FASTSCAN_COMMAND_ID_A);
    }

    bool VDXAPIENTRY ExecuteMenu(int id, VDXHWND hwndParent) override {
        if (id != 0) {
            return false;
        }
        return gController.Execute(hwndParent, callbacks_);
    }

    bool VDXAPIENTRY TranslateMessage(MSG& msg) override {
        return gController.TranslateMessage(msg);
    }

    bool VDXAPIENTRY HandleError(const char*, int, void*) override {
        return false;
    }

    bool VDXAPIENTRY HandleFileOpen(const wchar_t*, const wchar_t*, VDXHWND) override {
        gController.HandleFileOpen();
        return false;
    }

    void VDXAPIENTRY Attach(VDXHWND hwndParent) override {
        gController.Attach(hwndParent, callbacks_);
    }

    void VDXAPIENTRY Detach(VDXHWND) override {
        gController.Shutdown();
    }

    bool VDXAPIENTRY HandleFileOpenError(const wchar_t*, const wchar_t*, VDXHWND,
                                         const char*, int) override {
        gController.HandleFileOpen();
        return false;
    }

private:
    std::atomic<long> referenceCount_{0};
    IVDToolCallbacks* callbacks_ = nullptr;
};

bool VDXAPIENTRY CreateFastScan(const VDXToolContext* context,
                                IVDXTool** result) {
    if (!context || !context->mpCallbacks || !result) {
        return false;
    }

    auto* tool = new (std::nothrow) FastScanTool(context);
    if (!tool) {
        return false;
    }

    tool->AddRef();
    *result = tool;
    return true;
}

VDXToolDefinition gToolDefinition = {
    sizeof(VDXToolDefinition),
    &CreateFastScan
};

bool VDXAPIENTRY ShowStaticAbout(VDXHWND parent) {
    MessageBoxW(
        ToHWND(parent),
        FASTSCAN_PLUGIN_NAME_W L"\n\n"
        L"Click 500, 2000, or 3000 immediately after Mark Out.\n"
        L"Enter or a click (not a drag) in the input/output video pane pauses/continues the selected speed.\n"
        L"A click on the timeline stops the scan.",
        FASTSCAN_PLUGIN_NAME_W,
        MB_OK | MB_ICONINFORMATION);
    return true;
}

VDXPluginInfo gPluginInfo = {
    sizeof(VDXPluginInfo),
    FASTSCAN_PLUGIN_NAME_W,
    L"Science&Motion / OpenAI",
    FASTSCAN_PLUGIN_DESCRIPTION_W,
    0x0100000A,
    kVDXPluginType_Tool,
    0,
    11,
    kVDXPlugin_APIVersion,
    kVDXPlugin_ToolAPIVersion,
    kVDXPlugin_ToolAPIVersion,
    &gToolDefinition,
    &ShowStaticAbout,
    nullptr
};

const VDPluginInfo* const gPlugins[] = {
    &gPluginInfo,
    nullptr
};

} // namespace

extern "C" __declspec(dllexport) const VDPluginInfo* const* VDXAPIENTRY VDGetPluginInfo() {
    return gPlugins;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
