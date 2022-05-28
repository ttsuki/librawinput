/// @file
/// @brief  librawinput
/// @author ttsuki
/// @date   2019.10.23 original
/// @date   2021.11.18 v2
/// @date   2022.05.22 v3

// Licensed under the MIT License.
// Copyright (c) 2019-2022 ttsuki All rights reserved.

#include "librawinput.h"

#include <Windows.h>

#include <hidusage.h>
#include <hidsdi.h>
#include <hidpi.h>

#undef min
#undef max

#include <cstddef>
#include <chrono>
#include <array>
#include <cassert>
#include <vector>
#include <optional>
#include <functional>

#include <map>
#include <thread>
#include <future>

#pragma comment(lib, "hid.lib")

namespace ttsuki::librawinput
{
    /// Threaded win32 message window
    class ThreadedMessageWindow final
    {
    public:
        using WndProc = std::function<std::optional<LRESULT>(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)>;

    private:
        WndProc windowProc_{};
        std::thread thread_{};
        DWORD threadId_{};
        HWND window_{};

    public:
        [[nodiscard]] DWORD ThreadId() const { return threadId_; }
        [[nodiscard]] HWND Window() const { return window_; }

        ThreadedMessageWindow(const char* className, const char* windowName, WndProc wndProc)
            : windowProc_(std::move(wndProc))
        {
            struct SubThreadData
            {
                DWORD threadId_{};
                HWND window_{};
            };

            std::promise<SubThreadData> promise;
            std::future<SubThreadData> future = promise.get_future();

            thread_ = std::thread([this, ready = std::move(promise), className, windowName]() mutable
            {
                (void)::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // STAThread
                (void)::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

                auto createParams = this;
                auto windowClass = RegisterMessageWindowClass(className, NativeWndProc);
                auto window = CreateMessageWindow(windowClass.get(), windowName, createParams);

                // Allocates message queue.
                MSG msg{};
                ::PeekMessageA(&msg, nullptr, 0, 0, PM_NOREMOVE);

                // Notifies ready to parent thread.
                ready.set_value(SubThreadData{::GetCurrentThreadId(), window.get()});

                // Run message loop.
                while (::GetMessageA(&msg, nullptr, 0, 0))
                {
                    ::DispatchMessageA(&msg);
                }
            });

            // Waits for ready.
            {
                const SubThreadData ready = future.get();
                this->threadId_ = ready.threadId_;
                this->window_ = ready.window_;
            }
        }

        ~ThreadedMessageWindow()
        {
            ::PostThreadMessageA(threadId_, WM_QUIT, 0, 0); // exit message loop
            thread_.join();
        }

        ThreadedMessageWindow(const ThreadedMessageWindow& other) = delete;
        ThreadedMessageWindow(ThreadedMessageWindow&& other) noexcept = delete;
        ThreadedMessageWindow& operator=(const ThreadedMessageWindow& other) = delete;
        ThreadedMessageWindow& operator=(ThreadedMessageWindow&& other) noexcept = delete;

        LRESULT SendMessageToWindow(UINT msg, WPARAM wParam, LPARAM lParam)
        {
            return ::SendMessageA(window_, msg, wParam, lParam);
        }

        BOOL PostMessageToWindow(UINT msg, WPARAM wParam, LPARAM lParam)
        {
            return ::PostMessageA(window_, msg, wParam, lParam);
        }

    protected:
        static LRESULT CALLBACK NativeWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_CREATE)
                ::SetWindowLongPtrA(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<LPCREATESTRUCTA>(lParam)->lpCreateParams));

            if (auto this_ = reinterpret_cast<ThreadedMessageWindow*>(::GetWindowLongPtrA(hWnd, GWLP_USERDATA)))
                if (auto returnValue = this_->windowProc_(hWnd, message, wParam, lParam))
                    return *returnValue;

            return ::DefWindowProcA(hWnd, message, wParam, lParam);
        }

        static std::shared_ptr<std::remove_pointer_t<LPCSTR>> RegisterMessageWindowClass(const char* className, WNDPROC pfnWndProc)
        {
            WNDCLASSEXA wcx = {sizeof(WNDCLASSEXA)};
            wcx.lpfnWndProc = pfnWndProc;
            wcx.hInstance = ::GetModuleHandleA(nullptr);
            wcx.lpszClassName = className;

            ATOM atom = ::RegisterClassExA(&wcx);
            return {
                reinterpret_cast<LPCSTR>(atom),
                [instance = wcx.hInstance](LPCSTR p) { ::UnregisterClassA(p, instance); }
            };
        }

        static std::shared_ptr<std::remove_pointer_t<HWND>> CreateMessageWindow(LPCSTR className, LPCSTR windowName, void* createParams)
        {
            return {
                ::CreateWindowExA(
                    0, className, windowName, 0,
                    0, 0, 0, 0,
                    HWND_MESSAGE,
                    nullptr,
                    ::GetModuleHandleA(nullptr),
                    createParams),
                ::DestroyWindow
            };
        }
    };

    std::vector<RawInputDeviceDescription> GetRawInputDeviceList(RawInputDeviceType targetDevices)
    {
        constexpr UINT RawInputError = static_cast<UINT>(-1);

        UINT deviceCount = 0;
        if (::GetRawInputDeviceList(nullptr, &deviceCount, static_cast<UINT>(sizeof(RAWINPUTDEVICELIST))) == RawInputError)
        {
            return {};
        }

        std::vector<RAWINPUTDEVICELIST> connectedDevices(deviceCount);
        if (::GetRawInputDeviceList(connectedDevices.data(), &deviceCount, static_cast<UINT>(sizeof(RAWINPUTDEVICELIST))) == RawInputError)
        {
            // retry
            return GetRawInputDeviceList(targetDevices);
        }

        std::vector<RawInputDeviceDescription> result;

        for (RAWINPUTDEVICELIST device : connectedDevices)
        {
            RawInputDeviceDescription dev{};
            dev.Handle = device.hDevice;
            RID_DEVICE_INFO i{};
            {
                std::vector<wchar_t> buf(256);
                UINT size = static_cast<UINT>(buf.size());
                ::GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, buf.data(), &size);
                dev.Path = buf.data();
            }

            {
                UINT size = sizeof(i);
                if (::GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO, &i, &size) == RawInputError)
                    continue;

                if (i.dwType == RIM_TYPEMOUSE)
                {
                    dev.Type = RawInputDeviceType::Mouse;
                }
                else if (i.dwType == RIM_TYPEKEYBOARD)
                {
                    dev.Type = RawInputDeviceType::Keyboard;
                }
                else if (i.dwType == RIM_TYPEHID && i.hid.usUsagePage == HID_USAGE_PAGE_GENERIC && i.hid.usUsage == HID_USAGE_GENERIC_JOYSTICK)
                {
                    dev.Type = RawInputDeviceType::Joystick;
                }
                else if (i.dwType == RIM_TYPEHID && i.hid.usUsagePage == HID_USAGE_PAGE_GENERIC && i.hid.usUsage == HID_USAGE_GENERIC_GAMEPAD)
                {
                    dev.Type = RawInputDeviceType::GamePad;
                }
                else
                {
                    // not supported device.
                    continue;
                }

                if ((dev.Type & targetDevices) == RawInputDeviceType::None)
                {
                    // not target device.
                    continue;
                }
            }

            {
                std::wstring Path{};
                HANDLE hFile = CreateFileW(
                    dev.Path.c_str(), 0, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, NULL, nullptr);
                if (hFile != INVALID_HANDLE_VALUE)
                {
                    std::vector<wchar_t> manufacture(256);
                    std::vector<wchar_t> product(256);
                    std::vector<wchar_t> serialNumber(256);

                    (void)::HidD_GetManufacturerString(hFile, manufacture.data(), static_cast<ULONG>(manufacture.size()));
                    (void)::HidD_GetProductString(hFile, product.data(), static_cast<ULONG>(product.size()));
                    (void)::HidD_GetSerialNumberString(hFile, serialNumber.data(), static_cast<ULONG>(serialNumber.size()));

                    ::CloseHandle(hFile);

                    dev.ManufactureName = manufacture.data();
                    dev.ProductName = product.data();
                    dev.SerialNumber = serialNumber.data();
                }
            }

            result.push_back(dev);
        }

        return result;
    }

    KeyboardEvent KeyboardEvent::Parse(const RAWINPUT* raw, TIMESTAMP timestamp)
    {
        KeyboardEvent e{};
        e.Device = raw->header.hDevice;
        e.Timestamp = timestamp;
        e.RawKeyboard = raw->data.keyboard;
        return e;
    }

    MouseEvent MouseEvent::Parse(const RAWINPUT* raw, TIMESTAMP timestamp)
    {
        MouseEvent e{};
        e.Device = raw->header.hDevice;
        e.Timestamp = timestamp;
        e.RawMouse = raw->data.mouse;
        return e;
    }

    HidEvent HidEvent::Parse(const RAWINPUT* raw, TIMESTAMP timestamp)
    {
        class DeviceCaps
        {
        public:
            HIDP_CAPS caps{};
            std::vector<HIDP_VALUE_CAPS> valueCaps{};
            std::vector<HIDP_BUTTON_CAPS> buttonCaps{};
            std::unique_ptr<std::byte[]> preparsedData{};

            static std::unique_ptr<DeviceCaps> LoadDeviceCaps(HANDLE hDevice)
            {
                auto caps = std::make_unique<DeviceCaps>();
                UINT requiredBufferSize = 0;

                if (::GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, nullptr, &requiredBufferSize) != 0)
                {
                    ::OutputDebugStringA("Failed to GetRawInputDeviceInfo(...)");
                    if (::IsDebuggerPresent()) ::DebugBreak();
                    return nullptr;
                }

                caps->preparsedData = std::make_unique<std::byte[]>(requiredBufferSize);
                auto preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(caps->preparsedData.get());
                if (UINT result = requiredBufferSize; ::GetRawInputDeviceInfoW(hDevice, RIDI_PREPARSEDDATA, preparsed, &result) != requiredBufferSize)
                {
                    ::OutputDebugStringA("Failed to GetRawInputDeviceInfo(...)");
                    if (::IsDebuggerPresent()) ::DebugBreak();
                    return nullptr;
                }

                if (::HidP_GetCaps(preparsed, &caps->caps) == HIDP_STATUS_SUCCESS)
                {
                    if (USHORT size = 0; ::HidP_GetValueCaps(HidP_Input, nullptr, &size, preparsed) == HIDP_STATUS_BUFFER_TOO_SMALL)
                    {
                        caps->valueCaps.resize(size);
                        (void)::HidP_GetValueCaps(HidP_Input, caps->valueCaps.data(), &size, preparsed);
                    }

                    if (USHORT size = 0; ::HidP_GetButtonCaps(HidP_Input, nullptr, &size, preparsed) == HIDP_STATUS_BUFFER_TOO_SMALL)
                    {
                        caps->buttonCaps.resize(size);
                        (void)::HidP_GetButtonCaps(HidP_Input, caps->buttonCaps.data(), &size, preparsed);
                    }
                }
                else
                {
                    ::OutputDebugStringA("Failed to HidP_GetCaps(...)");
                    if (::IsDebuggerPresent()) ::DebugBreak();
                    return nullptr;
                }

                return caps;
            }

            static const DeviceCaps* GetDeviceCaps(HANDLE hDevice)
            {
                thread_local std::map<HANDLE, std::unique_ptr<DeviceCaps>> deviceCapsCache_;
                auto it = deviceCapsCache_.find(hDevice);
                if (it == deviceCapsCache_.end())
                {
                    it = deviceCapsCache_.emplace(hDevice, LoadDeviceCaps(hDevice)).first;
                }
                return it->second.get();
            }
        };

        HidEvent e{};
        e.Device = raw->header.hDevice;
        e.Timestamp = timestamp;

        // Load device caps
        if (const DeviceCaps* deviceCaps = DeviceCaps::GetDeviceCaps(raw->header.hDevice))
        {
            const auto preparsedData = reinterpret_cast<PHIDP_PREPARSED_DATA>(deviceCaps->preparsedData.get());
            const auto rawData = reinterpret_cast<PCHAR>(const_cast<RAWINPUT*>(raw)->data.hid.bRawData);
            const auto rawDataLen = raw->data.hid.dwSizeHid;

            // value input
            for (size_t valueCount = std::min(deviceCaps->valueCaps.size(), e.Values.capacity()), i = 0; i < valueCount; i++)
            {
                const HIDP_VALUE_CAPS& cap = deviceCaps->valueCaps[i];
                ULONG value{};
                if (::HidP_GetUsageValue(
                    HidP_Input,
                    cap.UsagePage, 0, cap.NotRange.Usage,
                    &value,
                    preparsedData, rawData, rawDataLen) == HIDP_STATUS_SUCCESS)
                {
                    e.Values.push_back({
                        cap.UsagePage,
                        cap.NotRange.Usage,
                        static_cast<int32_t>(value),
                        static_cast<int32_t>(cap.LogicalMin),
                        static_cast<int32_t>(cap.LogicalMax),
                    });
                }
            }

            // button input
            for (size_t buttonPageCount = std::min(deviceCaps->buttonCaps.size(), e.Buttons.capacity()), i = 0; i < buttonPageCount; i++)
            {
                const HIDP_BUTTON_CAPS& cap = deviceCaps->buttonCaps[i];

                USAGE usage[kMaxCountOfButtonsPerPage]{};
                ULONG len = _countof(usage);

                if (::HidP_GetUsages(
                    HidP_Input,
                    cap.UsagePage, 0,
                    usage, &len,
                    preparsedData, rawData, rawDataLen) == HIDP_STATUS_SUCCESS)
                {
                    // pressed button to bit-index
                    const USAGE buttonsInPage = cap.IsRange ? cap.Range.UsageMax - cap.Range.UsageMin + 1 : cap.NotRange.Usage;

                    uint64_t buttonPressed{};
                    for (size_t j = 0; j < len; j++)
                    {
                        const USAGE index = usage[j] - cap.Range.UsageMin;
                        if (index <= buttonsInPage && index < kMaxCountOfButtonsPerPage)
                        {
                            buttonPressed |= 1ULL << index;
                        }
                    }

                    e.Buttons.push_back({cap.UsagePage, static_cast<uint16_t>(buttonsInPage), buttonPressed,});
                }
            }
        }

        return e;
    }

    JoystickHidEvent JoystickHidEvent::FromHidEvent(const HidEvent& e)
    {
        JoystickHidEvent r{e.Device, e.Timestamp};

        const auto axis = [](const HidEvent::ValueInput& v)-> float
        {
            float value = static_cast<float>(v.Value);
            float minValue = static_cast<float>(v.MinValue);
            float maxValue = static_cast<float>(v.MaxValue);
            float centerValue = (maxValue - minValue) / 2.0f;
            return std::clamp((value - centerValue) / centerValue, -1.0f, 1.0f);
        };

        const auto throttle = [](const HidEvent::ValueInput& v)-> std::optional<float>
        {
            float value = static_cast<float>(v.Value);
            float minValue = static_cast<float>(v.MinValue);
            float maxValue = static_cast<float>(v.MaxValue);

            float valueRange = maxValue - minValue;

            if (minValue <= value && value <= maxValue)
            {
                return std::clamp((value - minValue) / valueRange, 0.0f, 1.0f);
            }

            return std::nullopt;
        };

        int sliderCount = 0;
        int hatSwitchCount = 0;
        constexpr float PI2 = 2.0f * 3.141592653589793238462643383279502884f;
        for (auto&& value : e.Values)
        {
            if (value.Page == HID_USAGE_PAGE_GENERIC)
            {
                switch (value.Usage)
                {
                case HID_USAGE_GENERIC_X:
                    r.X = axis(value);
                    break;
                case HID_USAGE_GENERIC_Y:
                    r.Y = axis(value);
                    break;
                case HID_USAGE_GENERIC_Z:
                    r.Z = axis(value);
                    break;
                case HID_USAGE_GENERIC_RX:
                    r.RotX = axis(value);
                    break;
                case HID_USAGE_GENERIC_RY:
                    r.RotY = axis(value);
                    break;
                case HID_USAGE_GENERIC_RZ:
                    r.RotZ = axis(value);
                    break;

                case HID_USAGE_GENERIC_SLIDER:
                    switch (sliderCount++)
                    {
                    case 0:
                        r.Slider0 = throttle(value);
                        break;
                    case 1:
                        r.Slider1 = throttle(value);
                        break;
                    case 2:
                        r.Slider2 = throttle(value);
                        break;
                    case 3:
                        r.Slider3 = throttle(value);
                        break;
                    default: // ignore
                        break;
                    }
                    break;

                case HID_USAGE_GENERIC_HATSWITCH:
                    switch (hatSwitchCount++)
                    {
                    case 0:
                        r.HatSwitch0 = throttle(value);
                        r.HatSwitch0X = r.HatSwitch0 ? std::cos(*r.HatSwitch0 * PI2) : 0.0f;
                        r.HatSwitch0Y = r.HatSwitch0 ? std::sin(*r.HatSwitch0 * PI2) : 0.0f;
                        break;
                    case 1:
                        r.HatSwitch1 = throttle(value);
                        r.HatSwitch1X = r.HatSwitch1 ? std::cos(*r.HatSwitch1 * PI2) : 0.0f;
                        r.HatSwitch1Y = r.HatSwitch1 ? std::sin(*r.HatSwitch1 * PI2) : 0.0f;
                        break;
                    default: // ignore
                        break;
                    }
                    break;

                default: // ignore value
                    break;
                }
            }

            if (value.Page == HID_USAGE_PAGE_SIMULATION)
            {
                switch (value.Usage)
                {
                case HID_USAGE_SIMULATION_STEERING:
                    r.X = axis(value);
                    break;
                case HID_USAGE_SIMULATION_ACCELLERATOR:
                    r.Y = axis(value);
                    break;
                case HID_USAGE_SIMULATION_BRAKE:
                    r.Z = axis(value);
                    break;
                case HID_USAGE_SIMULATION_RUDDER:
                    r.RotZ = axis(value);
                    break;
                case HID_USAGE_SIMULATION_THROTTLE:
                    r.Slider0 = throttle(value);
                    break;
                default:
                    break;
                }
            }

            if (value.Page == HID_USAGE_PAGE_GAME)
            {
                switch (value.Usage)
                {
                case HID_USAGE_GAME_POINT_OF_VIEW:
                    r.HatSwitch0 = throttle(value);
                    r.HatSwitch0X = r.HatSwitch0 ? std::cos(*r.HatSwitch0 * PI2) : 0.0f;
                    r.HatSwitch0Y = r.HatSwitch0 ? std::sin(*r.HatSwitch0 * PI2) : 0.0f;
                    break;
                default:
                    break;
                }
            }
        }

        uint32_t buttonIndex = 0;
        uint64_t buttonStatus = 0;
        for (auto&& p : e.Buttons)
        {
            if (p.Page == HID_USAGE_PAGE_BUTTON)
            {
                buttonStatus |= p.ButtonStatuses << buttonIndex;
                buttonIndex += p.ButtonCount;
            }

            if (buttonIndex >= r.Buttons.size()) break;
        }
        r.ButtonCount = buttonIndex;
        r.Buttons |= std::bitset<64>(buttonStatus);

        return r;
    }

    class RawInputEventListenerImpl final
    {
        RawInputDeviceType targetDeviceTypes_{};
        RawInputEventCallback rawEventCallback_{};
        KeyboardEventCallback keyboardEventCallback_{};
        MouseEventCallback mouseEventCallback_{};
        HidEventCallback hidEventCallback_{};
        JoystickHidEventCallback joystickHidEventCallback_{};
        std::unique_ptr<ThreadedMessageWindow> messageWindow_;

        static inline constexpr UINT WM_REGISTER_DEVICE = WM_APP + 1;

    public:
        RawInputEventListenerImpl(RawInputDeviceType deviceType, RawInputCallbacks callbacks)
            : targetDeviceTypes_(deviceType)
            , rawEventCallback_(std::move(callbacks.RawInputEventCallback))
            , keyboardEventCallback_(std::move(callbacks.KeyboardEventCallback))
            , mouseEventCallback_(std::move(callbacks.MouseEventCallback))
            , hidEventCallback_(std::move(callbacks.HidEventCallback))
            , joystickHidEventCallback_(std::move(callbacks.JoystickHidEventCallback))
            , messageWindow_(std::make_unique<ThreadedMessageWindow>(
                  "CLSRawInputEventListener",
                  "WNDRawInputEventListener",
                  [this]([[maybe_unused]] HWND hWnd,
                         [[maybe_unused]] UINT message,
                         [[maybe_unused]] WPARAM wParam,
                         [[maybe_unused]] LPARAM lParam) -> std::optional<LRESULT>
                  {
                      switch (message)
                      {
                      case WM_REGISTER_DEVICE: return this->RegisterDevices(static_cast<DWORD>(wParam), reinterpret_cast<HWND>(lParam));
                      case WM_INPUT: return this->ProcessWMInput(reinterpret_cast<HRAWINPUT>(lParam));
                      default: return std::nullopt;
                      }
                  }))
        {
            // Starts event listening.
            messageWindow_->PostMessageToWindow(WM_REGISTER_DEVICE, RIDEV_INPUTSINK, reinterpret_cast<LPARAM>(messageWindow_->Window()));
        }

        ~RawInputEventListenerImpl()
        {
            // Stops event listening.
            messageWindow_->PostMessageToWindow(WM_REGISTER_DEVICE, RIDEV_REMOVE, reinterpret_cast<LPARAM>(nullptr));
        }

        RawInputEventListenerImpl(const RawInputEventListenerImpl& other) = delete;
        RawInputEventListenerImpl(RawInputEventListenerImpl&& other) noexcept = delete;
        RawInputEventListenerImpl& operator=(const RawInputEventListenerImpl& other) = delete;
        RawInputEventListenerImpl& operator=(RawInputEventListenerImpl&& other) noexcept = delete;

    private:
        LRESULT RegisterDevices(DWORD flags, HWND target)
        {
            FixedVector<RAWINPUTDEVICE, 16> v;
            using DevType = RawInputDeviceType;
            if (!!(targetDeviceTypes_ & DevType::Mouse)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_POINTER, flags, target});
            if (!!(targetDeviceTypes_ & DevType::Mouse)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, flags, target});
            if (!!(targetDeviceTypes_ & DevType::Joystick)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_JOYSTICK, flags, target});
            if (!!(targetDeviceTypes_ & DevType::GamePad)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_GAMEPAD, flags, target});
            if (!!(targetDeviceTypes_ & DevType::Keyboard)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, flags, target});
            if (!!(targetDeviceTypes_ & DevType::Keyboard)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYPAD, flags, target});
            if (!!(targetDeviceTypes_ & DevType::Joystick)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MULTI_AXIS_CONTROLLER, flags, target});

            if (BOOL rel = ::RegisterRawInputDevices(v.data(), static_cast<UINT>(v.size()), sizeof(RAWINPUTDEVICE)); !rel)
            {
                ::OutputDebugStringA("Failed to RegisterRawInputDevices(...)");
                if (::IsDebuggerPresent()) ::DebugBreak();
                return 1;
            }

            return 0;
        }

        LRESULT ProcessWMInput(HRAWINPUT hRawInput)
        {
            TIMESTAMP now = Clock();
            RAWINPUT* data = nullptr;

            // get input data
            std::array<std::byte, 4096> stack_buf{};
            std::unique_ptr<std::byte[]> heap_buf{};
            {
                void* buffer = stack_buf.data();
                UINT size = static_cast<UINT>(stack_buf.size());
                UINT result = ::GetRawInputData(hRawInput, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));

                // If stack_buf is not enough to store data, allocate heap_buf dynamically.
                if (result == static_cast<UINT>(-1))
                {
                    if (UINT required = 0; ::GetRawInputData(hRawInput, RID_INPUT, nullptr, &required, sizeof(RAWINPUTHEADER)) == 0)
                    {
                        heap_buf = std::make_unique<std::byte[]>(required);
                        buffer = heap_buf.get();
                        size = required;
                        result = ::GetRawInputData(hRawInput, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));
                    }
                }

                // Failed...?
                if (result == static_cast<UINT>(-1))
                {
                    ::OutputDebugStringA("Failed to GetRawInputData(...)\n");
                    if (::IsDebuggerPresent()) ::DebugBreak();
                    return 1;
                }

                data = static_cast<RAWINPUT*>(buffer);
            }

            // Raises input event callback.

            if (rawEventCallback_)
            {
                rawEventCallback_(data, now);
            }

            if (keyboardEventCallback_ && data->header.dwType == RIM_TYPEKEYBOARD)
            {
                KeyboardEvent e = KeyboardEvent::Parse(data, now);
                keyboardEventCallback_(e);
            }

            if (mouseEventCallback_ && data->header.dwType == RIM_TYPEMOUSE)
            {
                MouseEvent e = MouseEvent::Parse(data, now);
                mouseEventCallback_(e);
            }

            if ((hidEventCallback_ || joystickHidEventCallback_) && data->header.dwType == RIM_TYPEHID)
            {
                HidEvent e = HidEvent::Parse(data, now);
                if (hidEventCallback_)
                {
                    hidEventCallback_(e);
                }

                if (joystickHidEventCallback_)
                {
                    JoystickHidEvent r = JoystickHidEvent::FromHidEvent(e);
                    joystickHidEventCallback_(r);
                }
            }
            return 0;
        }
    };

    std::shared_ptr<void> StartRawInput(RawInputDeviceType targetDevices, RawInputCallbacks callbacks)
    {
        return std::make_shared<RawInputEventListenerImpl>(targetDevices, callbacks);
    }
}
