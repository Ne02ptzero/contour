/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/InputGenerator.h>
#include <terminal/OutputGenerator.h>
#include <terminal/Terminal.h>
#include <terminal/Process.h>
#include <terminal/UTF8.h>
#include <terminal/Util.h>

#include <cstdio>
#include <cctype>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <variant>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "FontManager.h"
#include "Window.h"
#include "GLTerminal.h"
#include "GLLogger.h"
#include "Flags.h"

#if defined(__linux__)
#include <fontconfig/fontconfig.h>
#endif

// TODOs:
// - [x] proper glterm termination (window close as well as process exit)
// - [x] input: rename Numpad_Dot to Numpad_Decimal, and others (Div -> Divide, etc)
// - [x] Fix window-resize: call Screen::resize(), PseudoTerminal::updateWindowSize()
// - [x] logging: runtime-configurable logging (to file or stdout, differ between error/warn/debug/trace logging)
// - [x] Hi-DPI support (hm, because I need it)
// - [ ] Fix font size on non-Hi-DPI screens (such as my Linux monitor)
// - [ ] show cursor (in correct shapes, with blinking)
//   - [x] CursorShape: Block
//   - [ ] Screen: HideCursor / ShowCursor
//   - [ ] CursorShape: Beam
//   - [ ] CursorShape: Underline
//   - [ ] Blinking Mode
// - [ ] basic runtime-reloadable config file (yaml?)
// - [ ] input: fix input sequences on non ConPTY hosts (cursor keys, ...?)
// - [ ] other SGRs (bold, italic, etc)
// - [ ] Windowed fullscreen support (ALT+ENTER, or similar)
// - [ ] fix text positioning (chars seem pressed down instead of centered)
// - [ ] font (fontconfig) loading on Linux
// - [ ] input: F13..F25
// - [ ] input: GLFW_KEY_PRINT_SCREEN
// - [ ] input: GLFW_KEY_PAUSE
// - [ ] input: GLFW_KEY_KP_EQUAL

#if defined(__unix__)
#include <unistd.h>
#endif

using namespace std;
using namespace std::placeholders;

class GLTerm {
  public:
    GLTerm(
        terminal::WindowSize const& _winSize,
        unsigned short _fontSize,
        std::string const& _fontFamily,
        CursorShape _cursorShape,
        glm::vec3 const& _cursorColor,
        std::string const& _shell,
        LogMask _logMask);

    ~GLTerm();

    int main();

  private:
    void render();
    void onResize(unsigned _width, unsigned _height);
    void onKey(int _key, int _scanCode, int _action, int _mods);
    void onChar(char32_t _char);
    void onContentScale(float _xs, float _ys);

  private:
    GLLogger logger_;
    FontManager fontManager_;
    Font& regularFont_;
    Window window_;
    GLTerminal terminalView_;
};

GLTerm::GLTerm(terminal::WindowSize const& _winSize,
               unsigned short _fontSize,
               std::string const& _fontFamily,
               CursorShape _cursorShape,
               glm::vec3 const& _cursorColor,
               std::string const& _shell,
               LogMask _logMask) :
    //loggingSink_{"glterm.log", ios::trunc},
    logger_{_logMask, &cout},
    fontManager_{},
    regularFont_{
        fontManager_.load(
            _fontFamily,
            static_cast<unsigned>(_fontSize * Window::primaryMonitorContentScale().second)
        )
    },
    window_{
        _winSize.columns * regularFont_.maxAdvance(),
        _winSize.rows * regularFont_.lineHeight(),
        "glterm",
        bind(&GLTerm::onKey, this, _1, _2, _3, _4),
        bind(&GLTerm::onChar, this, _1),
        bind(&GLTerm::onResize, this, _1, _2),
        bind(&GLTerm::onContentScale, this, _1, _2)
    },
    terminalView_{
        _winSize,
        window_.width(),
        window_.height(),
        regularFont_,
        _cursorShape,
        _cursorColor,
        _shell,
        glm::ortho(0.0f, static_cast<GLfloat>(window_.width()), 0.0f, static_cast<GLfloat>(window_.height())),
        logger_
    }
{
    if (!regularFont_.isFixedWidth())
        throw runtime_error{ "Regular font is not a fixed-width font." };
    glViewport(0, 0, window_.width(), window_.height());
}

GLTerm::~GLTerm()
{
}

int GLTerm::main()
{
    while (terminalView_.alive() && !glfwWindowShouldClose(window_))
    {
        if (terminalView_.shouldRender())
            render();

        glfwWaitEventsTimeout(0.5);
    }

    return EXIT_SUCCESS;
}

void GLTerm::render()
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    terminalView_.render();

    glfwSwapBuffers(window_);
}

void GLTerm::onContentScale(float _xs, float _ys)
{
    cout << fmt::format("Updated content scale to: {:.2f} by {:.2f}\n", _xs, _ys);
    // TODO: scale fontSize by factor _ys.
}

void GLTerm::onResize(unsigned _width, unsigned _height)
{
    glViewport(0, 0, _width, _height);
    terminalView_.setProjection(glm::ortho(0.0f, static_cast<GLfloat>(_width), 0.0f, static_cast<GLfloat>(_height)));
    terminalView_.resize(_width, _height);
    render();
}

optional<terminal::Key> glfwKeyToTerminalKey(int _key)
{
    using terminal::Key;

    static auto constexpr mapping = array{
        make_pair(GLFW_KEY_ESCAPE, Key::Escape),
        make_pair(GLFW_KEY_ENTER, Key::Enter),
        make_pair(GLFW_KEY_TAB, Key::Tab),
        make_pair(GLFW_KEY_BACKSPACE, Key::Backspace),
        make_pair(GLFW_KEY_INSERT, Key::Insert),
        make_pair(GLFW_KEY_DELETE, Key::Delete),
        make_pair(GLFW_KEY_RIGHT, Key::RightArrow),
        make_pair(GLFW_KEY_LEFT, Key::LeftArrow),
        make_pair(GLFW_KEY_DOWN, Key::DownArrow),
        make_pair(GLFW_KEY_UP, Key::UpArrow),
        make_pair(GLFW_KEY_PAGE_DOWN, Key::PageDown),
        make_pair(GLFW_KEY_PAGE_UP, Key::PageUp),
        make_pair(GLFW_KEY_HOME, Key::Home),
        make_pair(GLFW_KEY_END, Key::End),
        // TODO: some of those below...
        //#define GLFW_KEY_CAPS_LOCK          280
        //#define GLFW_KEY_SCROLL_LOCK        281
        //#define GLFW_KEY_NUM_LOCK           282
        //#define GLFW_KEY_PRINT_SCREEN       283
        //#define GLFW_KEY_PAUSE              284
        make_pair(GLFW_KEY_F1, Key::F1),
        make_pair(GLFW_KEY_F2, Key::F2),
        make_pair(GLFW_KEY_F3, Key::F3),
        make_pair(GLFW_KEY_F4, Key::F4),
        make_pair(GLFW_KEY_F5, Key::F5),
        make_pair(GLFW_KEY_F6, Key::F6),
        make_pair(GLFW_KEY_F7, Key::F7),
        make_pair(GLFW_KEY_F8, Key::F8),
        make_pair(GLFW_KEY_F9, Key::F9),
        make_pair(GLFW_KEY_F10, Key::F10),
        make_pair(GLFW_KEY_F11, Key::F11),
        make_pair(GLFW_KEY_F12, Key::F12),
        // todo: F13..F25
        make_pair(GLFW_KEY_KP_0, Key::Numpad_0),
        make_pair(GLFW_KEY_KP_1, Key::Numpad_1),
        make_pair(GLFW_KEY_KP_2, Key::Numpad_2),
        make_pair(GLFW_KEY_KP_3, Key::Numpad_3),
        make_pair(GLFW_KEY_KP_4, Key::Numpad_4),
        make_pair(GLFW_KEY_KP_5, Key::Numpad_5),
        make_pair(GLFW_KEY_KP_6, Key::Numpad_6),
        make_pair(GLFW_KEY_KP_7, Key::Numpad_7),
        make_pair(GLFW_KEY_KP_8, Key::Numpad_8),
        make_pair(GLFW_KEY_KP_9, Key::Numpad_9),
        make_pair(GLFW_KEY_KP_DECIMAL, Key::Numpad_Decimal),
        make_pair(GLFW_KEY_KP_DIVIDE, Key::Numpad_Divide),
        make_pair(GLFW_KEY_KP_MULTIPLY, Key::Numpad_Multiply),
        make_pair(GLFW_KEY_KP_SUBTRACT, Key::Numpad_Subtract),
        make_pair(GLFW_KEY_KP_ADD, Key::Numpad_Add),
        make_pair(GLFW_KEY_KP_ENTER, Key::Numpad_Enter),
        make_pair(GLFW_KEY_KP_EQUAL, Key::Numpad_Equal),
        #if 0
        #define GLFW_KEY_LEFT_SHIFT         340
        #define GLFW_KEY_LEFT_CONTROL       341
        #define GLFW_KEY_LEFT_ALT           342
        #define GLFW_KEY_LEFT_SUPER         343
        #define GLFW_KEY_RIGHT_SHIFT        344
        #define GLFW_KEY_RIGHT_CONTROL      345
        #define GLFW_KEY_RIGHT_ALT          346
        #define GLFW_KEY_RIGHT_SUPER        347
        #define GLFW_KEY_MENU               348
        #endif
    };

    if (auto i = find_if(begin(mapping), end(mapping), [_key](auto const& x) { return x.first == _key; }); i != end(mapping))
        return { i->second };

    return nullopt;
}

constexpr terminal::Modifier makeModifier(int _mods)
{
    using terminal::Modifier;

    Modifier mods{};

    if (_mods & GLFW_MOD_ALT)
        mods |= Modifier::Alt;
    if (_mods & GLFW_MOD_SHIFT)
        mods |= Modifier::Shift;
    if (_mods & GLFW_MOD_CONTROL)
        mods |= Modifier::Control;
    if (_mods & GLFW_MOD_SUPER)
        mods |= Modifier::Meta;

    return mods;
}

void GLTerm::onKey(int _key, int _scanCode, int _action, int _mods)
{
    if (_action == GLFW_PRESS || _action == GLFW_REPEAT)
    {
        terminal::Modifier const mods = makeModifier(_mods);

        char const* keyName = glfwGetKeyName(_key, _scanCode);

        logger_.keyTrace(fmt::format(
            "key: {} {}, action:{}, mod:{:02X} ({})",
            _key,
            keyName ? keyName : "(null)",
            _action,
            static_cast<unsigned>(_mods),
            terminal::to_string(mods)));

        // Screenshot: ALT+CTRL+S
        if (_key == GLFW_KEY_S && mods == (terminal::Modifier::Control + terminal::Modifier::Alt))
        {
            auto const screenshot = terminalView_.screenshot();
            ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
            ofs << screenshot;
            return;
        }

        if (auto const key = glfwKeyToTerminalKey(_key); key.has_value())
            terminalView_.send(key.value(), mods);
        else if (const char* cstr = glfwGetKeyName(_key, _scanCode);
                cstr != nullptr && strlen(cstr) == 1
            && mods.some() && mods != terminal::Modifier::Shift
            && isalnum(*cstr))
        {
            // allow only mods + alphanumerics
            terminalView_.send(*cstr, mods);
        }
        //else if (mods && mods != terminal::Modifier::Shift)
        //    logger_(UnsupportedInputMappingEvent{fmt::format(
        //        "key:{}, scanCode:{}, name:{} ({})",
        //        _key, _scanCode, cstr, terminal::to_string(mods)
        //    )});
    }
}

void GLTerm::onChar(char32_t _char)
{
    terminalView_.send(_char, terminal::Modifier{});
}

CursorShape makeCursorShape(string const& _name)
{
    if (_name == "block")
        return CursorShape::Block;

    if (_name == "underscore")
        return CursorShape::Underscore;

    if (_name == "beam")
        return CursorShape::Beam;

    throw runtime_error("Invalid cursor shape. Use one of block, underscore, beam.");
}

int main(int argc, char const* argv[])
{
    try
    {
        util::Flags flags;
        flags.defineBool("help", 'h', "Shows this help and quits.");
        flags.defineBool("log-parser-error", 0, "Enables logging of parser errorrs.");
        flags.defineBool("log-raw-input", 0, "Enables logging of raw input.");
        flags.defineBool("log-raw-output", 0, "Enables logging of raw output.");
        flags.defineBool("log-invalid-output", 0, "Enables logging of invalid output sequences.");
        flags.defineBool("log-unsupported-output", 0, "Enables logging of unsupported output sequences.");
        flags.defineBool("log-trace-output", 0, "Enables logging of output trace.");
        flags.defineNumber("font-size", 'S', "PIXELS", "Defines character font-size.", 12);
        flags.defineNumber("columns", 'C', "COUNT", "Defines number of text columns.", 130);
        flags.defineNumber("lines", 'L', "COUNT", "Defines number of text lines.", 25);
        flags.defineString("font", 'F', "PATTERN", "Defines font family.", "Fira Code, Ubuntu Mono, Consolas, monospace");
        flags.defineString("cursor-shape", 'P', "SHAPE", "Defines cursor shape.", "block");
        flags.defineString("shell", 's', "SHELL", "Defines shell to invoke.", terminal::Process::loginShell());

        flags.parse(argc, argv);

        LogMask const logMask = [&]() {
            LogMask mask{};
            if (flags.getBool("log-parser-error"))
                mask |= LogMask::ParserError;

            if (flags.getBool("log-invalid-output"))
                mask |= LogMask::InvalidOutput;

            if (flags.getBool("log-unsupported-output"))
                mask |= LogMask::UnsupportedOutput;

            if (flags.getBool("log-raw-input"))
                mask |= LogMask::RawInput;

            if (flags.getBool("log-raw-output"))
                mask |= LogMask::RawOutput;

            if (flags.getBool("log-trace-output"))
                mask |= LogMask::TraceOutput;

            return mask;
        }();

        if (flags.getBool("help"))
        {
            cout << "glterm - Terminal Emulator.\n"
                 << "\n"
                 << "Usage:\n"
                 << "  glterm [OPTIONS ...]\n"
                 << "\n"
                 << flags.helpText() << endl;
            return EXIT_SUCCESS;
        }

        auto const cursorColor = glm::vec3{ 1.0, 1.0, 0.0 };

        auto glterm = GLTerm{
            terminal::WindowSize{
                static_cast<unsigned short>(flags.getNumber("columns")),
                static_cast<unsigned short>(flags.getNumber("lines"))
            },
            static_cast<unsigned short>(flags.getNumber("font-size")),
            flags.getString("font"),
            makeCursorShape(flags.getString("cursor-shape")),
            cursorColor,
            flags.getString("shell"),
            logMask
        };
        return glterm.main();
    }
    catch (exception const& e)
    {
        cerr << "Unhandled error caught. " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
