/// @file
/// @brief  librawinput test program.
/// @author ttsuki

// Licensed under the MIT License.
// Copyright (c) 2019-2022 ttsuki All rights reserved.

#include "librawinput.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <bitset>
#include <thread>
#include <algorithm>
#include <future>

int main()
{
    using namespace ttsuki::librawinput;

    RawInputDeviceType targets{};
    targets |= RawInputDeviceType::Mouse;
    targets |= RawInputDeviceType::Keyboard;
    targets |= RawInputDeviceType::Joystick;
    targets |= RawInputDeviceType::GamePad;

    // Shows connected devices.
    {
        std::cout << "Connected devices: " << std::endl;

        auto devices = GetRawInputDeviceList(targets);
        std::sort(devices.begin(), devices.end(), [](const RawInputDeviceDescription& a, RawInputDeviceDescription& b)
        {
            if (a.Type != b.Type) return a.Type < b.Type;
            return a.Path < b.Path;
        });

        for (auto&& device : devices)
        {
            using namespace std;

            cout << " - device=" << "0x" << hex << setfill('0') << setw(8) << device.Handle << dec;
            cout << " type=" << [](RawInputDeviceType i)
            {
                switch (i)
                {
                case RawInputDeviceType::None: return "None";
                case RawInputDeviceType::Mouse: return "Mouse";
                case RawInputDeviceType::Keyboard: return "Keyboard";
                case RawInputDeviceType::Joystick: return "Joystick";
                case RawInputDeviceType::GamePad: return "GamePad";
                case RawInputDeviceType::Other: return "Other";
                case RawInputDeviceType::ALL: return "ALL";
                default: return "?";
                }
            }(device.Type);
            wcout << L" Path=" << (!device.Path.empty() ? device.Path : L"(empty)");
            if (!device.ManufactureName.empty()) wcout << L" ManufactureName=" << device.ManufactureName;
            if (!device.ProductName.empty()) wcout << L" ProductName=" << device.ProductName;
            if (!device.SerialNumber.empty()) wcout << L" SerialNumber=" << device.SerialNumber;
            cout << std::endl;
        }
    }

    std::promise<void> escape_key_pressed_promise;
    std::future<void> escape_key_pressed = escape_key_pressed_promise.get_future();

    // Create callbacks

    RawInputCallbacks callbacks{};

    callbacks.KeyboardEventCallback = [&escape_key_pressed_promise](const KeyboardEvent& e)
    {
        using namespace std;
        ostringstream oss;
        oss << " time=" << setprecision(6) << fixed << (static_cast<double>(e.Timestamp) / 1000000.0);
        oss << " Keyboard";
        oss << " device=" << "0x" << e.Device;
        oss << " vkey=" << hex << std::setw(2) << e.VirtualKeyCode() << dec;
        oss << " " << (e.KeyIsDown() ? "down" : "up");
        oss << "\n";
        cout << oss.str();

        if (e.VirtualKeyCode() == VK_ESCAPE)
        {
            escape_key_pressed_promise.set_value();
        }
    };

    callbacks.MouseEventCallback = [](const MouseEvent& e)
    {
        using namespace std;
        ostringstream oss;
        oss << " time=" << setprecision(6) << fixed << (static_cast<double>(e.Timestamp) / 1000000.0);
        oss << " Mouse";
        oss << " device=" << "0x" << e.Device;
        oss << " " << (e.LastXYIsAbsolute() ? "absolute" : "relative");
        oss << " position=" << e.LastX() << "," << e.LastY();
        if (e.WheelDelta()) oss << " wheel=" << e.WheelDelta();
        oss << " buttons=";
        oss << (e.ButtonIsDown(MouseEvent::ButtonIndex::Button1) ? "1" : e.ButtonIsUp(MouseEvent::ButtonIndex::Button1) ? "x" : "_");
        oss << (e.ButtonIsDown(MouseEvent::ButtonIndex::Button2) ? "2" : e.ButtonIsUp(MouseEvent::ButtonIndex::Button2) ? "x" : "_");
        oss << (e.ButtonIsDown(MouseEvent::ButtonIndex::Button3) ? "3" : e.ButtonIsUp(MouseEvent::ButtonIndex::Button3) ? "x" : "_");
        oss << (e.ButtonIsDown(MouseEvent::ButtonIndex::Button4) ? "4" : e.ButtonIsUp(MouseEvent::ButtonIndex::Button4) ? "x" : "_");
        oss << (e.ButtonIsDown(MouseEvent::ButtonIndex::Button5) ? "5" : e.ButtonIsUp(MouseEvent::ButtonIndex::Button5) ? "x" : "_");
        oss << "\n";
        cout << oss.str();
    };

    callbacks.JoystickHidEventCallback = [](const JoystickHidEvent& e)
    {
        using namespace std;
        ostringstream oss;
        oss << " time=" << setprecision(6) << fixed << (static_cast<double>(e.Timestamp) / 1000000.0);
        oss << " Joystick";
        oss << " device=" << "0x" << e.Device;

        oss << setprecision(3) << fixed << showpos;
        if (e.X) oss << " X=" << *e.X;
        if (e.Y) oss << " Y=" << *e.Y;
        if (e.Z) oss << " Z=" << *e.Z;
        if (e.RotX) oss << " Rx=" << *e.RotX;
        if (e.RotY) oss << " Ry=" << *e.RotY;
        if (e.RotZ) oss << " Rz=" << *e.RotZ;
        if (e.Slider0) oss << " S0=" << *e.Slider0;
        if (e.Slider1) oss << " S1=" << *e.Slider1;
        if (e.HatSwitch0) oss << " HS0=" << *e.HatSwitch0;
        if (e.HatSwitch1) oss << " HS1=" << *e.HatSwitch1;
        if (e.HatSwitch0X) oss << " HS0X=" << *e.HatSwitch0X;
        if (e.HatSwitch0Y) oss << " HS0Y=" << *e.HatSwitch0Y;
        if (e.HatSwitch1X) oss << " HS1X=" << *e.HatSwitch1X;
        if (e.HatSwitch1Y) oss << " HS1Y=" << *e.HatSwitch1Y;

        auto btn = e.Buttons.to_string('_', '1');
        std::reverse(btn.begin(), btn.end());
        btn = btn.substr(0, e.ButtonCount);
        oss << " Buttons(count=" << e.ButtonCount << ")=" << btn;
        oss << "\n";
        cout << oss.str();
    };

    callbacks.RawInputEventCallback = [](const RAWINPUT* raw, TIMESTAMP timestamp)
    {
        using namespace std;

        if (raw->header.dwType == RIM_TYPEKEYBOARD)
        {
            ::OutputDebugStringA((
                ostringstream()
                << "K"
                << " time = " << timestamp
                << hex << setfill('0') << " device=0x" << setw(8) << raw->header.hDevice
                << hex << setfill('0') << " MakeCode=" << setw(4) << raw->data.keyboard.MakeCode
                << hex << setfill('0') << " Flags=" << setw(4) << raw->data.keyboard.Flags
                << hex << setfill('0') << " Reserved=" << setw(4) << raw->data.keyboard.Reserved
                << hex << setfill('0') << " VKey=" << setw(4) << raw->data.keyboard.VKey
                << hex << setfill('0') << " Message=" << setw(4) << raw->data.keyboard.Message
                << hex << setfill('0') << " ExtraInformation=" << setw(8) << raw->data.keyboard.ExtraInformation
                << "\n"
            ).str().c_str());
        }

        if (raw->header.dwType == RIM_TYPEMOUSE)
        {
            ::OutputDebugStringA((
                ostringstream()
                << "M"
                << " time = " << timestamp
                << hex << setfill('0') << " device=0x" << setw(8) << raw->header.hDevice
                << hex << setfill('0') << " usFlags=" << setw(4) << raw->data.mouse.usFlags
                << hex << setfill('0') << " ulButtons=" << setw(8) << raw->data.mouse.ulButtons
                << hex << setfill('0') << " ulRawButtons=" << setw(8) << raw->data.mouse.ulRawButtons
                << dec << setfill(' ') << " lLastX=" << raw->data.mouse.lLastX
                << dec << setfill(' ') << " lLastY=" << raw->data.mouse.lLastY
                << hex << setfill('0') << " ulExtraInformation=" << setw(8) << raw->data.mouse.ulExtraInformation
                << "\n"
            ).str().c_str());
        }

        if (raw->header.dwType == RIM_TYPEHID)
        {
            ::OutputDebugStringA((
                ostringstream()
                << "H"
                << " time = " << timestamp
                << hex << setfill('0') << " device=0x" << setw(8) << raw->header.hDevice << dec
                << dec << setfill(' ') << " dwSizeHid=" << raw->data.hid.dwSizeHid
                << dec << setfill(' ') << " dwCount=" << raw->data.hid.dwCount
                << [&]
                {
                    ostringstream os;
                    for (size_t i = 0; i < raw->data.hid.dwCount; i++)
                    {
                        os << "data[" << dec << setw(0) << i << "]=";
                        for (size_t j = 0; j < raw->data.hid.dwSizeHid; j++)
                            os << hex << setw(2) << setfill('0') << static_cast<uint32_t>(raw->data.hid.bRawData[i * raw->data.hid.dwSizeHid + j]);
                    }
                    return os.str();
                }()
                << "\n"
            ).str().c_str());
        }
    };

    // Starts listening Raw Input events.

    std::cout << "Initializing RawInput event sink..." << std::endl;
    auto rawInputListener = StartRawInput(targets, callbacks);
    std::cout << "Ready. Press ESCAPE to exit." << std::endl;

    escape_key_pressed.wait();

    std::cout << "Finalizing..." << std::endl;
    rawInputListener.reset();
    std::cout << "Finalized." << std::endl;

    return 0;
}
