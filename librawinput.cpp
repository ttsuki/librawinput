/// @file
/// @brief  librawinput
/// @author ttsuki
/// @date   2019.10.23 original
/// @date   2021.11.18 v2
/// @date   2022.05.22 v3

// Licensed under the MIT License.
// Copyright (c) 2019-2023 ttsuki All rights reserved.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "librawinput.h"

#include <Windows.h>
#include <hidusage.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <cstddef>
#include <cstdint>
#include <cassert>
#include <memory>
#include <chrono>
#include <string>
#include <array>
#include <vector>
#include <unordered_map>
#include <optional>
#include <bitset>
#include <functional>
#include <thread>
#include <future>

#pragma comment(lib, "hid.lib")

namespace ttsuki::librawinput
{
    /// Threaded win32 message window
    class ThreadedMessageWindow final
    {
    public:
        using WndProc = std::function<std::optional<LRESULT>(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)>;

    private:
        WndProc window_proc_{};
        std::thread thread_{};
        DWORD thread_id_{};
        HWND window_{};

    public:
        [[nodiscard]] DWORD ThreadId() const { return thread_id_; }
        [[nodiscard]] HWND Window() const { return window_; }

        ThreadedMessageWindow(LPCSTR lpClassName, LPCSTR lpWindowName, WndProc wnd_proc)
            : window_proc_(std::move(wnd_proc))
        {
            struct SubThreadData
            {
                DWORD thread_id{};
                HWND window{};
            };

            std::promise<SubThreadData> promise;
            std::future<SubThreadData> future = promise.get_future();

            thread_ = std::thread([this, ready = std::move(promise), lpClassName, lpWindowName]() mutable
            {
                (void)::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

                auto create_param = this;
                auto wnd_class = RegisterMessageWindowClass(lpClassName, NativeWndProc);
                auto wnd_handle = CreateMessageWindow(wnd_class.get(), lpWindowName, create_param);

                // Allocates message queue.
                MSG msg{};
                ::PeekMessageA(&msg, nullptr, 0, 0, PM_NOREMOVE);

                // Notifies ready to parent thread.
                ready.set_value(SubThreadData{::GetCurrentThreadId(), wnd_handle.get()});

                // Run message loop.
                while (::GetMessageA(&msg, nullptr, 0, 0))
                {
                    ::DispatchMessageA(&msg);
                }
            });

            // Waits for ready.
            {
                const SubThreadData ready = future.get();
                this->thread_id_ = ready.thread_id;
                this->window_ = ready.window;
            }
        }

        ~ThreadedMessageWindow()
        {
            ::PostThreadMessageA(thread_id_, WM_QUIT, 0, 0); // exit message loop
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
        static std::shared_ptr<std::remove_pointer_t<LPCSTR>> RegisterMessageWindowClass(LPCSTR lpClassName, WNDPROC pfnWndProc)
        {
            WNDCLASSEXA wcx = {sizeof(WNDCLASSEXA)};
            wcx.lpfnWndProc = pfnWndProc;
            wcx.hInstance = ::GetModuleHandleA(nullptr);
            wcx.lpszClassName = lpClassName;

            ATOM atom = ::RegisterClassExA(&wcx);
            return {
                reinterpret_cast<LPCSTR>(atom),
                [instance = wcx.hInstance](LPCSTR p) { if (p) { ::UnregisterClassA(p, instance); } }
            };
        }

        static std::shared_ptr<std::remove_pointer_t<HWND>> CreateMessageWindow(LPCSTR lpClassName, LPCSTR lpWindowName, void* lpCreateParams)
        {
            return {
                ::CreateWindowExA(
                    0, lpClassName, lpWindowName, 0,
                    0, 0, 0, 0,
                    HWND_MESSAGE,
                    nullptr,
                    ::GetModuleHandleA(nullptr),
                    lpCreateParams),
                ::DestroyWindow
            };
        }

        static LRESULT CALLBACK NativeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            if (msg == WM_CREATE)
            {
                // Store WndProc
                auto desc = reinterpret_cast<LPCREATESTRUCTA>(lParam);
                auto host = static_cast<ThreadedMessageWindow*>(desc->lpCreateParams);
                SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
            }

            // Load WndProc
            if (auto host = reinterpret_cast<ThreadedMessageWindow*>(::GetWindowLongPtrA(hWnd, GWLP_USERDATA)))
                if (auto result = host->window_proc_(hWnd, msg, wParam, lParam))
                    return *result;

            return ::DefWindowProcA(hWnd, msg, wParam, lParam);
        }
    };

    std::vector<RawInputDeviceDescription> GetRawInputDeviceList(RawInputDeviceType target_device_types)
    {
        constexpr UINT RAW_INPUT_ERROR = static_cast<UINT>(-1);

        UINT count = 0;
        if (UINT ret = ::GetRawInputDeviceList(nullptr, &count, static_cast<UINT>(sizeof(RAWINPUTDEVICELIST)));
            ret == RAW_INPUT_ERROR)
        {
            return {};
        }

        std::vector<RAWINPUTDEVICELIST> connected_devices(count);
        if (UINT ret = ::GetRawInputDeviceList(connected_devices.data(), &count, static_cast<UINT>(sizeof(RAWINPUTDEVICELIST)));
            ret == RAW_INPUT_ERROR)
        {
            // retry
            return GetRawInputDeviceList(target_device_types);
        }
        else
        {
            connected_devices.resize(ret);
        }

        std::vector<RawInputDeviceDescription> result;

        for (const RAWINPUTDEVICELIST& device : connected_devices)
        {
            RawInputDeviceDescription dev{};
            dev.Handle = device.hDevice;

            RID_DEVICE_INFO device_info{};

            {
                std::vector<wchar_t> buf(1024, L'\0');

                UINT size = static_cast<UINT>(buf.size());
                if (UINT ret = ::GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, buf.data(), &size);
                    ret == RAW_INPUT_ERROR)
                    continue; // not connected.

                dev.Path = buf.data(); // chomp
            }

            {
                UINT size = sizeof(device_info);
                if (UINT ret = ::GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICEINFO, &device_info, &size);
                    ret == RAW_INPUT_ERROR)
                    continue;

                if (device_info.dwType == RIM_TYPEMOUSE)
                {
                    dev.Type = RawInputDeviceType::Mouse;
                }
                else if (device_info.dwType == RIM_TYPEKEYBOARD)
                {
                    dev.Type = RawInputDeviceType::Keyboard;
                }
                else if (device_info.dwType == RIM_TYPEHID && (device_info.hid.usUsagePage == HID_USAGE_PAGE_GENERIC && device_info.hid.usUsage == HID_USAGE_GENERIC_JOYSTICK))
                {
                    dev.Type = RawInputDeviceType::Joystick;
                }
                else if (device_info.dwType == RIM_TYPEHID && (device_info.hid.usUsagePage == HID_USAGE_PAGE_GENERIC && device_info.hid.usUsage == HID_USAGE_GENERIC_GAMEPAD))
                {
                    dev.Type = RawInputDeviceType::GamePad;
                }
                else if (device_info.dwType == RIM_TYPEHID)
                {
                    dev.Type = RawInputDeviceType::Other;
                }

                if ((dev.Type & target_device_types) == RawInputDeviceType::None)
                {
                    // not target device.
                    continue;
                }
            }

            if (HANDLE hFile = CreateFileW(dev.Path.c_str(), 0, FILE_SHARE_READ, nullptr, OPEN_EXISTING, NULL, nullptr);
                hFile != INVALID_HANDLE_VALUE)
            {
                std::vector<wchar_t> manufacture(256);
                std::vector<wchar_t> product(256);
                std::vector<wchar_t> serial_number(256);

                (void)::HidD_GetManufacturerString(hFile, manufacture.data(), static_cast<ULONG>(manufacture.size()));
                (void)::HidD_GetProductString(hFile, product.data(), static_cast<ULONG>(product.size()));
                (void)::HidD_GetSerialNumberString(hFile, serial_number.data(), static_cast<ULONG>(serial_number.size()));

                ::CloseHandle(hFile);

                dev.ManufactureName = manufacture.data();
                dev.ProductName = product.data();
                dev.SerialNumber = serial_number.data();
            }

            result.push_back(dev);
        }

        return result;
    }

    struct HidDeviceCaps
    {
        HANDLE DeviceHandle{};
        HIDP_CAPS HidPCaps{};
        std::unique_ptr<std::byte[]> PreparsedDataBlob{};
        std::vector<HIDP_VALUE_CAPS> ValueCaps{};
        std::vector<HIDP_BUTTON_CAPS> ButtonCaps{};
        [[nodiscard]] PHIDP_PREPARSED_DATA PreparsedData() const noexcept { return reinterpret_cast<PHIDP_PREPARSED_DATA>(PreparsedDataBlob.get()); }

        static std::unique_ptr<HidDeviceCaps> FromDevice(HANDLE device);
    };

    class RawInputEventListenerImpl final
    {
        static inline constexpr UINT WM_REGISTER_DEVICE = WM_APP + 1;

        RawInputDeviceType target_device_types_{};
        RawInputCallbacks callbacks_{};
        std::unique_ptr<ThreadedMessageWindow> message_window_{};
        std::unordered_map<HANDLE, std::unique_ptr<HidDeviceCaps>> preparsed_data_cache_{};

    public:
        RawInputEventListenerImpl(RawInputDeviceType target_device_types, RawInputCallbacks callbacks)
            : target_device_types_(target_device_types)
            , callbacks_(std::move(callbacks))
            , message_window_(std::make_unique<ThreadedMessageWindow>(
                  "CLSRawInputEventListener",
                  "WNDRawInputEventListener",
                  [this]([[maybe_unused]] HWND hWnd,
                         [[maybe_unused]] UINT uMsg,
                         [[maybe_unused]] WPARAM wParam,
                         [[maybe_unused]] LPARAM lParam) -> std::optional<LRESULT>
                  {
                      switch (uMsg)
                      {
                      case WM_REGISTER_DEVICE: return this->RegisterDevices(static_cast<DWORD>(wParam), reinterpret_cast<HWND>(lParam));
                      case WM_INPUT: return this->ProcessWMInput(reinterpret_cast<HRAWINPUT>(lParam));
                      default: return std::nullopt;
                      }
                  }))
        {
            auto list = GetRawInputDeviceList(target_device_types);
            for (const auto& desc : list)
                preparsed_data_cache_[desc.Handle] = HidDeviceCaps::FromDevice(desc.Handle);

            // Starts event listening.
            message_window_->PostMessageToWindow(WM_REGISTER_DEVICE, RIDEV_INPUTSINK, reinterpret_cast<LPARAM>(message_window_->Window()));
        }

        ~RawInputEventListenerImpl()
        {
            // Stops event listening.
            message_window_->PostMessageToWindow(WM_REGISTER_DEVICE, RIDEV_REMOVE, reinterpret_cast<LPARAM>(nullptr));
        }

        RawInputEventListenerImpl(const RawInputEventListenerImpl& other) = delete;
        RawInputEventListenerImpl(RawInputEventListenerImpl&& other) noexcept = delete;
        RawInputEventListenerImpl& operator=(const RawInputEventListenerImpl& other) = delete;
        RawInputEventListenerImpl& operator=(RawInputEventListenerImpl&& other) noexcept = delete;

    private:
        LRESULT RegisterDevices(DWORD flags, HWND target)
        {
            ARRAY<RAWINPUTDEVICE, 16> v;
            using DevType = RawInputDeviceType;
            if (!!(target_device_types_ & DevType::Other)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_POINTER, flags, target});
            if (!!(target_device_types_ & DevType::Mouse)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MOUSE, flags, target});
            if (!!(target_device_types_ & DevType::Keyboard)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYBOARD, flags, target});
            if (!!(target_device_types_ & DevType::Joystick)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_JOYSTICK, flags, target});
            if (!!(target_device_types_ & DevType::GamePad)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_GAMEPAD, flags, target});
            if (!!(target_device_types_ & DevType::Other)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_KEYPAD, flags, target});
            if (!!(target_device_types_ & DevType::Other)) v.push_back(RAWINPUTDEVICE{HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_MULTI_AXIS_CONTROLLER, flags, target});

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

            // get input data
            auto input_data_buffer = [stack_buf = std::array<std::byte, 4096>(), heap_buf = std::unique_ptr<std::byte[]>()](HRAWINPUT hRawInput) mutable -> RAWINPUT*
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
                    return nullptr;
                }

                return static_cast<RAWINPUT*>(buffer);
            };

            RAWINPUT* data = input_data_buffer(hRawInput);

            // Raises input event callback.

            if (callbacks_.RawInputEventCallback)
            {
                callbacks_.RawInputEventCallback(data, now);
            }

            if (callbacks_.KeyboardEventCallback && data->header.dwType == RIM_TYPEKEYBOARD)
            {
                KeyboardEvent e = KeyboardEvent::Parse(data, now);
                callbacks_.KeyboardEventCallback(e);
            }

            if (callbacks_.MouseEventCallback && data->header.dwType == RIM_TYPEMOUSE)
            {
                MouseEvent e = MouseEvent::Parse(data, now);
                callbacks_.MouseEventCallback(e);
            }

            if ((callbacks_.HidEventCallback || callbacks_.JoystickHidEventCallback) && data->header.dwType == RIM_TYPEHID)
            {
                if (auto it = preparsed_data_cache_.find(data->header.hDevice);
                    it != preparsed_data_cache_.end() && it->second)
                {
                    HidEvent e = HidEvent::Parse(data, now, it->second.get());
                    if (callbacks_.HidEventCallback)
                    {
                        callbacks_.HidEventCallback(e);
                    }

                    if (callbacks_.JoystickHidEventCallback)
                    {
                        JoystickHidEvent r = JoystickHidEvent::FromHidEvent(e);
                        callbacks_.JoystickHidEventCallback(r);
                    }
                }
            }
            return 0;
        }
    };

    std::shared_ptr<void> StartRawInput(RawInputDeviceType target_device_types, RawInputCallbacks callbacks)
    {
        return std::make_shared<RawInputEventListenerImpl>(target_device_types, callbacks);
    }

    KeyboardEvent KeyboardEvent::Parse(const RAWINPUT* input, TIMESTAMP timestamp)
    {
        KeyboardEvent e{};
        e.Device = input->header.hDevice;
        e.Timestamp = timestamp;
        e.RawKeyboard = input->data.keyboard;
        return e;
    }

    MouseEvent MouseEvent::Parse(const RAWINPUT* input, TIMESTAMP timestamp)
    {
        MouseEvent e{};
        e.Device = input->header.hDevice;
        e.Timestamp = timestamp;
        e.RawMouse = input->data.mouse;
        return e;
    }

    std::unique_ptr<HidDeviceCaps> HidDeviceCaps::FromDevice(HANDLE device)
    {
        std::unique_ptr<HidDeviceCaps> caps = std::make_unique<HidDeviceCaps>();
        caps->DeviceHandle = device;

        UINT buf_size = 0;
        if (::GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, nullptr, &buf_size) != 0)
        {
            ::OutputDebugStringA("Failed to GetRawInputDeviceInfo(...)");
            if (::IsDebuggerPresent()) ::DebugBreak();
            return nullptr;
        }

        caps->PreparsedDataBlob = std::make_unique<std::byte[]>(buf_size);
        if (UINT expected = buf_size;
            ::GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, caps->PreparsedDataBlob.get(), &buf_size) != expected)
        {
            ::OutputDebugStringA("Failed to GetRawInputDeviceInfo(...)");
            if (::IsDebuggerPresent()) ::DebugBreak();
            return nullptr;
        }

        if (buf_size > 0)
        {
            auto preparsed = caps->PreparsedData();
            if (::HidP_GetCaps(preparsed, &caps->HidPCaps) == HIDP_STATUS_SUCCESS)
            {
                if (USHORT size = 0; ::HidP_GetValueCaps(HidP_Input, nullptr, &size, preparsed) == HIDP_STATUS_BUFFER_TOO_SMALL)
                {
                    caps->ValueCaps.resize(size);
                    (void)::HidP_GetValueCaps(HidP_Input, caps->ValueCaps.data(), &size, preparsed);
                }

                if (USHORT size = 0; ::HidP_GetButtonCaps(HidP_Input, nullptr, &size, preparsed) == HIDP_STATUS_BUFFER_TOO_SMALL)
                {
                    caps->ButtonCaps.resize(size);
                    (void)::HidP_GetButtonCaps(HidP_Input, caps->ButtonCaps.data(), &size, preparsed);
                }
            }
            else
            {
                ::OutputDebugStringA("Failed to HidP_GetCaps(...)");
                if (::IsDebuggerPresent()) ::DebugBreak();
                return nullptr;
            }
        }

        return caps;
    }

    HidEvent HidEvent::Parse(const RAWINPUT* input, TIMESTAMP timestamp, const HidDeviceCaps* caps)
    {
        HidEvent e{};
        e.Device = input->header.hDevice;
        e.Timestamp = timestamp;
        e.Caps = caps;

        if (caps)
        {
            const auto input_data = reinterpret_cast<PCHAR>(const_cast<RAWINPUT*>(input)->data.hid.bRawData);
            const auto input_size = input->data.hid.dwSizeHid;

            // process value input
            const size_t value_count = std::min(caps->ValueCaps.size(), e.Values.capacity());
            for (size_t i = 0; i < value_count; i++)
            {
                const HIDP_VALUE_CAPS& cap = caps->ValueCaps[i];
                ULONG value{};
                if (::HidP_GetUsageValue(
                    HidP_Input,
                    cap.UsagePage, 0, cap.NotRange.Usage, &value,
                    caps->PreparsedData(), input_data, input_size) == HIDP_STATUS_SUCCESS)
                {
                    e.Values.push_back({
                        static_cast<uint16_t>(cap.UsagePage),
                        static_cast<uint16_t>(cap.NotRange.Usage),
                        static_cast<int32_t>(value),
                        static_cast<int32_t>(cap.LogicalMin),
                        static_cast<int32_t>(cap.LogicalMax),
                        });
                }
            }

            // process button input
            const size_t button_page_count = std::min(caps->ButtonCaps.size(), e.Buttons.capacity());
            for (size_t i = 0; i < button_page_count; i++)
            {
                const HIDP_BUTTON_CAPS& cap = caps->ButtonCaps[i];
                USAGE usage[kMaxCountOfButtonsPerPage]{};
                ULONG len = static_cast<ULONG>(std::size(usage));

                if (::HidP_GetUsages(
                    HidP_Input,
                    cap.UsagePage, 0, usage, &len,
                    caps->PreparsedData(), input_data, input_size) == HIDP_STATUS_SUCCESS)
                {
                    // pressed button to bit-index
                    const USAGE first = cap.IsRange ? cap.Range.UsageMin : cap.NotRange.Usage;
                    const USAGE last = cap.IsRange ? cap.Range.UsageMax : cap.NotRange.Usage;
                    const size_t count = cap.IsRange ? last - first + 1 : 1;

                    uint64_t pressed_button_bit{};
                    for (size_t j = 0; j < len; j++)
                    {
                        const size_t index = usage[j] - first;
                        if (index <= count && index < kMaxCountOfButtonsPerPage)
                        {
                            pressed_button_bit |= 1ULL << index;
                        }
                    }

                    e.Buttons.push_back({
                        static_cast<uint16_t>(cap.UsagePage),
                        static_cast<uint16_t>(first),
                        static_cast<uint16_t>(last),
                        static_cast<uint16_t>(count),
                        pressed_button_bit
                        });
                }
            }
        }

        return e;
    }

    JoystickHidEvent JoystickHidEvent::FromHidEvent(const HidEvent& e)
    {
        JoystickHidEvent r{e.Device, e.Timestamp};

        const auto normalize_axis = [](const HidEvent::ValueInput& v)-> float
        {
            const float val = static_cast<float>(v.Value);
            const float min = static_cast<float>(v.MinValue);
            const float max = static_cast<float>(v.MaxValue);
            const float center = (max - min) / 2.0f;
            return std::clamp((val - center) / center, -1.0f, 1.0f);
        };

        const auto normalize_throttle = [](const HidEvent::ValueInput& v)-> std::optional<float>
        {
            const float val = static_cast<float>(v.Value);
            const float min = static_cast<float>(v.MinValue);
            const float max = static_cast<float>(v.MaxValue);
            const float ragne = max - min;

            if (min <= val && val <= max)
            {
                return std::clamp((val - min) / ragne, 0.0f, 1.0f);
            }

            return std::nullopt;
        };

        int slider_count = 0;
        int hat_switch_count = 0;
        const float PI2 = std::acos(-1.0f) * 2.0f;

        for (auto&& value : e.Values)
        {
            if (value.Page == HID_USAGE_PAGE_GENERIC)
            {
                switch (value.Usage)
                {
                case HID_USAGE_GENERIC_X:
                    r.X = normalize_axis(value);
                    break;
                case HID_USAGE_GENERIC_Y:
                    r.Y = normalize_axis(value);
                    break;
                case HID_USAGE_GENERIC_Z:
                    r.Z = normalize_axis(value);
                    break;
                case HID_USAGE_GENERIC_RX:
                    r.RotX = normalize_axis(value);
                    break;
                case HID_USAGE_GENERIC_RY:
                    r.RotY = normalize_axis(value);
                    break;
                case HID_USAGE_GENERIC_RZ:
                    r.RotZ = normalize_axis(value);
                    break;

                case HID_USAGE_GENERIC_SLIDER:
                    switch (slider_count++)
                    {
                    case 0:
                        r.Slider0 = normalize_throttle(value);
                        break;
                    case 1:
                        r.Slider1 = normalize_throttle(value);
                        break;
                    case 2:
                        r.Slider2 = normalize_throttle(value);
                        break;
                    case 3:
                        r.Slider3 = normalize_throttle(value);
                        break;
                    default: // ignore
                        break;
                    }
                    break;

                case HID_USAGE_GENERIC_HATSWITCH:
                    switch (hat_switch_count++)
                    {
                    case 0:
                        r.HatSwitch0 = normalize_throttle(value);
                        r.HatSwitch0X = r.HatSwitch0 ? std::cos(*r.HatSwitch0 * PI2) : 0.0f;
                        r.HatSwitch0Y = r.HatSwitch0 ? std::sin(*r.HatSwitch0 * PI2) : 0.0f;
                        break;
                    case 1:
                        r.HatSwitch1 = normalize_throttle(value);
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
                    r.X = normalize_axis(value);
                    break;
                case HID_USAGE_SIMULATION_ACCELLERATOR:
                    r.Y = normalize_axis(value);
                    break;
                case HID_USAGE_SIMULATION_BRAKE:
                    r.Z = normalize_axis(value);
                    break;
                case HID_USAGE_SIMULATION_RUDDER:
                    r.RotZ = normalize_axis(value);
                    break;
                case HID_USAGE_SIMULATION_THROTTLE:
                    r.Slider0 = normalize_throttle(value);
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
                    r.HatSwitch0 = normalize_throttle(value);
                    r.HatSwitch0X = r.HatSwitch0 ? std::cos(*r.HatSwitch0 * PI2) : 0.0f;
                    r.HatSwitch0Y = r.HatSwitch0 ? std::sin(*r.HatSwitch0 * PI2) : 0.0f;
                    break;
                default:
                    break;
                }
            }
        }

        uint32_t button_index = 0;
        uint64_t button_status = 0;
        for (auto&& p : e.Buttons)
        {
            if (p.Page == HID_USAGE_PAGE_BUTTON)
            {
                button_status |= p.ButtonStatuses << button_index;
                button_index += p.ButtonCount;
            }

            if (button_index >= r.Buttons.size()) break;
        }
        r.ButtonCount = button_index;
        r.Buttons |= std::bitset<64>(button_status);

        return r;
    }
}
