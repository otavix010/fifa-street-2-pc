// Development: FS2_PAD scripts the first controller, for tests without a person at the pad.
//   FS2_PAD="12:START,14.5:A,20-22:DOWN"  presses at a second (for 0.15 s), or holds over a range of seconds.
// Buttons: A B X Y BLACK WHITE LT RT START BACK UP DOWN LEFT RIGHT LS RS, sticks LX+ LX- LY+ LY- (full tilt).
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/input.h"
#include "core/log.h"

namespace game {

namespace {

struct Press {
    double from, to;
    std::string button;
};

std::vector<Press> Parse(const char* s) {
    std::vector<Press> out;
    std::string all = s;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t comma = all.find(',', pos);
        std::string item = all.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? all.size() : comma + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::string when = item.substr(0, colon);
        Press p;
        p.button = item.substr(colon + 1);
        size_t dash = when.find('-');
        p.from = std::atof(when.substr(0, dash).c_str());
        p.to = dash == std::string::npos ? p.from + 0.15 : std::atof(when.substr(dash + 1).c_str());
        out.push_back(p);
    }
    return out;
}

void Apply(const std::string& b, input::Pad& pad) {
    static const char* kDigital[] = {"UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LS", "RS"};
    static const char* kAnalog[] = {"A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT"};
    for (int i = 0; i < 8; i++)
        if (b == kDigital[i]) pad.buttons |= uint16_t(1 << i);
    for (int i = 0; i < 8; i++)
        if (b == kAnalog[i]) pad.analog[i] = 0xFF;
    if (b == "LX+") pad.lx = 32767;
    if (b == "LX-") pad.lx = -32768;
    if (b == "LY+") pad.ly = 32767;
    if (b == "LY-") pad.ly = -32768;
}

}  // namespace

void ScriptPad(int port, input::Pad& pad) {
    static const char* env = std::getenv("FS2_PAD");
    if (!env || port != 0) return;
    static const std::vector<Press> script = Parse(env);
    static const auto start = std::chrono::steady_clock::now();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    pad.connected = true;
    for (const Press& p : script)
        if (t >= p.from && t < p.to) {
            Apply(p.button, pad);
            LOG_FIRST(64, "pad script: %s at %.2f s", p.button.c_str(), t);
        }
}

}  // namespace game
