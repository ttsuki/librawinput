/// @file
/// @brief  librawinput
/// @author ttsuki
/// @date   2019.10.23 original
/// @date   2021.11.18 v2
/// @date   2022.05.22 v3

// Licensed under the MIT License.
// Copyright (c) 2019-2022 ttsuki All rights reserved.

#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <chrono>
#include <array>
#include <bitset>
#include <vector>
#include <optional>
#include <functional>

namespace ttsuki::librawinput
{
    /// Microsecond clock
    using TIMESTAMP = int64_t;

    static inline TIMESTAMP Clock()
    {
        using namespace std::chrono;
        static const high_resolution_clock::time_point ClockStart = high_resolution_clock::now();
        return static_cast<TIMESTAMP>(duration_cast<microseconds>(high_resolution_clock::now() - ClockStart).count());
    }

    enum struct RawInputDeviceType : uint32_t
    {
        None = 0x00,
        Mouse = 0x01,
        Keyboard = 0x02,
        Joystick = 0x04,
        GamePad = 0x08,
        ALL = ~0u,
    };

    struct RawInputDeviceDescription
    {
        HANDLE Handle{};
        RawInputDeviceType Type{};
        std::wstring Path{};
        std::wstring ManufactureName{};
        std::wstring ProductName{};
        std::wstring SerialNumber{};
    };

    /// Starts listening raw input events.
    /// @param targetDevices target devices (bitwise or-ed)
    /// @returns devices
    std::vector<RawInputDeviceDescription> GetRawInputDeviceList(RawInputDeviceType targetDevices);

    struct KeyboardEvent;
    struct MouseEvent;
    struct HidEvent;
    struct JoystickHidEvent;

    using RawInputEventCallback = std::function<void(const RAWINPUT* raw, TIMESTAMP timestamp)>;
    using KeyboardEventCallback = std::function<void(const KeyboardEvent&)>;
    using MouseEventCallback = std::function<void(const MouseEvent&)>;
    using HidEventCallback = std::function<void(const HidEvent&)>;
    using JoystickHidEventCallback = std::function<void(const JoystickHidEvent&)>;

    struct RawInputCallbacks
    {
        RawInputEventCallback RawInputEventCallback{};
        KeyboardEventCallback KeyboardEventCallback{};
        MouseEventCallback MouseEventCallback{};
        HidEventCallback HidEventCallback{};
        JoystickHidEventCallback JoystickHidEventCallback{};
    };

    /// Starts listening raw input events.
    /// @param targetDevices target devices (bitwise or-ed)
    /// @param callbacks event callbacks
    /// @returns listener
    std::shared_ptr<void> StartRawInput(RawInputDeviceType targetDevices, RawInputCallbacks callbacks);

    /// Fixed memory buffer vector template.
    template <class T, size_t TCapacity>
    struct FixedVector
    {
        std::array<T, TCapacity> values{};
        size_t count{};

        [[nodiscard]] const T* data() const { return values.data(); }
        [[nodiscard]] size_t size() const { return count; }
        [[nodiscard]] size_t capacity() const { return TCapacity; }
        [[nodiscard]] bool empty() const { return size() == 0; }
        [[nodiscard]] const T& operator [](size_t i) const { return values[i]; }
        [[nodiscard]] auto begin() const { return values.begin(); }
        [[nodiscard]] auto end() const { return values.begin() + count; }
        void clear() { count = 0; }
        void push_back(T item) { if (size() < capacity()) values[count++] = item; }
    };

    struct KeyboardEvent
    {
        /// Constructs KeyboardEvent from RAWINPUT.
        [[nodiscard]] static KeyboardEvent Parse(const RAWINPUT* raw, TIMESTAMP timestamp);

        HANDLE Device;
        TIMESTAMP Timestamp;
        RAWKEYBOARD RawKeyboard;

        [[nodiscard]] double ElapsedTimeSec() const { return static_cast<double>(Clock() - Timestamp) / 1000000.0; }
        [[nodiscard]] uint16_t VirtualKeyCode() const { return static_cast<uint16_t>(RawKeyboard.VKey); }
        [[nodiscard]] bool KeyIsDown() const { return (RawKeyboard.Flags & RI_KEY_BREAK) == 0; }
    };

    struct MouseEvent
    {
        /// Constructs MouseEvent from RAWINPUT.
        [[nodiscard]] static MouseEvent Parse(const RAWINPUT* raw, TIMESTAMP timestamp);

        HANDLE Device;
        TIMESTAMP Timestamp;
        RAWMOUSE RawMouse;

        enum struct ButtonIndex : uint32_t
        {
            Button1 = RI_MOUSE_LEFT_BUTTON_DOWN,
            Button2 = RI_MOUSE_RIGHT_BUTTON_DOWN,
            Button3 = RI_MOUSE_MIDDLE_BUTTON_DOWN,
            Button4 = RI_MOUSE_BUTTON_4_DOWN,
            Button5 = RI_MOUSE_BUTTON_5_DOWN,

            LeftButton = Button1,
            RightButton = Button2,
            MiddleButton = Button3,

            ButtonDownMask = Button1 | Button2 | Button3 | Button4 | Button5,
        };

        [[nodiscard]] double ElapsedTimeSec() const { return static_cast<double>(Clock() - Timestamp) / 1000000.0; }
        [[nodiscard]] int LastX() const { return RawMouse.lLastX; }
        [[nodiscard]] int LastY() const { return RawMouse.lLastY; }
        [[nodiscard]] bool LastXYIsAbsolute() const { return RawMouse.usFlags & MOUSE_MOVE_ABSOLUTE; }
        [[nodiscard]] int WheelDelta() const { return (RawMouse.usButtonFlags & RI_MOUSE_WHEEL) ? static_cast<int16_t>(RawMouse.usButtonData) : 0; }

        [[nodiscard]] ButtonIndex PressedButtons() const { return static_cast<ButtonIndex>(RawMouse.usButtonFlags & static_cast<uint32_t>(ButtonIndex::ButtonDownMask)); }
        [[nodiscard]] ButtonIndex ReleasedButtons() const { return static_cast<ButtonIndex>(RawMouse.usButtonFlags >> 1 & static_cast<uint32_t>(ButtonIndex::ButtonDownMask)); }
        [[nodiscard]] bool ButtonIsDown(ButtonIndex b) const { return (static_cast<uint32_t>(PressedButtons()) & static_cast<uint32_t>(b)) != 0; }
        [[nodiscard]] bool ButtonIsUp(ButtonIndex b) const { return (static_cast<uint32_t>(ReleasedButtons()) & static_cast<uint32_t>(b)) != 0; }
    };

    struct HidEvent
    {
        /// Constructs HidEvent from RAWINPUT.
        [[nodiscard]] static HidEvent Parse(const RAWINPUT* raw, TIMESTAMP timestamp);

        HANDLE Device;
        TIMESTAMP Timestamp;

        struct ValueInput
        {
            uint16_t Page;
            uint16_t Usage;
            int32_t Value;
            int32_t MinValue;
            int32_t MaxValue;
        };

        struct ButtonInput
        {
            uint16_t Page;
            uint16_t ButtonCount;
            uint64_t ButtonStatuses;
        };

        constexpr static const size_t kMaxCountOfValues = 16;
        constexpr static const size_t kMaxCountOfButtonPages = 16;
        constexpr static const size_t kMaxCountOfButtonsPerPage = 64;

        FixedVector<ValueInput, kMaxCountOfValues> Values;
        FixedVector<ButtonInput, kMaxCountOfButtonPages> Buttons;

        [[nodiscard]] double ElapsedTimeSec() const { return static_cast<double>(Clock() - Timestamp) / 1000000.0; }
    };

    struct JoystickHidEvent
    {
        /// Constructs JoystickHidEvent from HidEvent.
        [[nodiscard]] static JoystickHidEvent FromHidEvent(const HidEvent& e);

        HANDLE Device;
        TIMESTAMP Timestamp;
        std::optional<float> X, Y, Z;
        std::optional<float> RotX, RotY, RotZ;
        std::optional<float> Slider0, Slider1, Slider2, Slider3;
        std::optional<float> HatSwitch0, HatSwitch1;
        std::optional<float> HatSwitch0X, HatSwitch0Y;
        std::optional<float> HatSwitch1X, HatSwitch1Y;
        uint32_t ButtonCount;
        std::bitset<64> Buttons;

        [[nodiscard]] double ElapsedTimeSec() const { return static_cast<double>(Clock() - Timestamp) / 1000000.0; }
    };

    static inline std::underlying_type_t<RawInputDeviceType> operator +(RawInputDeviceType a) { return static_cast<std::underlying_type_t<RawInputDeviceType>>(a); }
    static inline bool operator !(RawInputDeviceType a) { return !+a; }
    static inline RawInputDeviceType operator ~(RawInputDeviceType a) { return static_cast<RawInputDeviceType>(~+a); }
    static inline RawInputDeviceType operator |(RawInputDeviceType a, RawInputDeviceType b) { return static_cast<RawInputDeviceType>(+a | +b); }
    static inline RawInputDeviceType operator &(RawInputDeviceType a, RawInputDeviceType b) { return static_cast<RawInputDeviceType>(+a & +b); }
    static inline RawInputDeviceType operator ^(RawInputDeviceType a, RawInputDeviceType b) { return static_cast<RawInputDeviceType>(+a ^ +b); }
    static inline RawInputDeviceType& operator |=(RawInputDeviceType& a, RawInputDeviceType b) { return a = a | b; }
    static inline RawInputDeviceType& operator &=(RawInputDeviceType& a, RawInputDeviceType b) { return a = a & b; }
    static inline RawInputDeviceType& operator ^=(RawInputDeviceType& a, RawInputDeviceType b) { return a = a ^ b; }
}
