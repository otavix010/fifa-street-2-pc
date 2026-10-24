#include "app/app.h"
#include "core/input.h"

namespace game {
void ScriptPad(int port, input::Pad& pad);  // pad_script.cpp
}

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"FIFA Street 2",                // window and splash
    .short_title = L"FIFA Street 2",          // message boxes
    .name = L"FIFAStreet2",                   // window classes, crash dumps, handover event
    .log = "fifast2",                         // log file prefix
    .settings = "settings.toml",              // settings file beside the exe
    .splash = L"splash",                      // optional splash.png or splash.bmp beside the exe
    .missing = L"Coloque o jogo na pasta assets: a imagem do disco (.iso), ou o default.xbe com a pasta data ao lado.",
    .images = {{"default.xbe", "FIFAStreet2"}},  // each recompiled xbe and its exe, for title launches
    .options = {},                            // game settings kept in the settings file
    .load = nullptr,                          // reads more from the settings file's toml::table
    .save = nullptr,                          // writes more into it
    .init = nullptr,                          // after the settings load, before the window and boot
    .loaded = nullptr,                        // image in memory, before its entry point runs
    .window = nullptr,                        // the game window is up
    .message = nullptr,                       // window messages first; true takes one
    .ui = nullptr,                            // ImGui drawing each frame
    .pad = game::ScriptPad,                   // changes a polled pad: FS2_PAD test scripts
    .rumble = nullptr,                        // changes a rumble before it is set
};
