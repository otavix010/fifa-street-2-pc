#pragma once
#include <cstdint>

namespace input {

// the console controller's report, analog buttons are 0-255
struct Pad {
    bool connected = false;
    uint16_t buttons = 0;  // dpad up/down/left/right, start, back, left thumb, right thumb
    uint8_t analog[8] = {};  // A B X Y black white left trigger right trigger
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

void Init();  // window thread, before boot; the game code window starts it earlier
uint16_t HeldOnAnyController();  // face buttons as XINPUT_GAMEPAD_A/B/X/Y masks, GameInfo::pad left out
// the controller on that port, then GameInfo::pad
Pad Poll(int port);
void Rumble(int port, uint16_t left, uint16_t right);

// Keyboard and mouse as a controller (keyboard.cpp): the window thread feeds them from its messages, and they join
// the pad on the port the settings give, which then reads as plugged in with or without a pad.
// A key by its position: the scan code in the message's lParam, with its extended bit
void Key(uint32_t scan, bool extended, bool down);
void MouseButton(int button, bool down);  // 1 left, 2 right, 3 middle, 4 and 5 the side ones
void MouseMove(int dx, int dy);           // raw counts, y down: the right stick
void ReleaseAll();                        // the window lost the keyboard: nothing stays held
bool MouseTaken();                        // the mouse plays (a key or a button of the layout was used): no cursor
void Merge(int port, Pad& pad);           // Poll's: the keys held into the pad of their port

}  // namespace input
