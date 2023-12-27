#include <iostream>
#include <io.h>
#include <fcntl.h>
#include "common.hpp"
#include "console_window.h"

namespace {
void BindCrtHandlesToStdHandles(bool bindStdIn, bool bindStdOut, bool bindStdErr) {
    // Re-initialize the C runtime "FILE" handles with clean handles bound to "nul". We do this because it has been
    // observed that the file number of our standard handle file objects can be assigned internally to a value of -2
    // when not bound to a valid target, which represents some kind of unknown internal invalid state. In this state our
    // call to "_dup2" fails, as it specifically tests to ensure that the target file number isn't equal to this value
    // before allowing the operation to continue. We can resolve this issue by first "re-opening" the target files to
    // use the "nul" device, which will place them into a valid state, after which we can redirect them to our target
    // using the "_dup2" function.
    if (bindStdIn) {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "r", stdin);
    }
    if (bindStdOut) {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "w", stdout);
    }
    if (bindStdErr) {
        FILE* dummyFile;
        freopen_s(&dummyFile, "nul", "w", stderr);
    }

    // Redirect unbuffered stdin from the current standard input handle
    if (bindStdIn) {
        HANDLE stdHandle = GetStdHandle(STD_INPUT_HANDLE);
        if (stdHandle != INVALID_HANDLE_VALUE) {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if (fileDescriptor != -1) {
                FILE* file = _fdopen(fileDescriptor, "r");
                if (file != NULL) {
                    int dup2Result = _dup2(_fileno(file), _fileno(stdin));
                    if (dup2Result == 0) {
                        setvbuf(stdin, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Redirect unbuffered stdout to the current standard output handle
    if (bindStdOut) {
        HANDLE stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (stdHandle != INVALID_HANDLE_VALUE) {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if (fileDescriptor != -1) {
                FILE* file = _fdopen(fileDescriptor, "w");
                if (file != NULL) {
                    int dup2Result = _dup2(_fileno(file), _fileno(stdout));
                    if (dup2Result == 0) {
                        setvbuf(stdout, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Redirect unbuffered stderr to the current standard error handle
    if (bindStdErr) {
        HANDLE stdHandle = GetStdHandle(STD_ERROR_HANDLE);
        if (stdHandle != INVALID_HANDLE_VALUE) {
            int fileDescriptor = _open_osfhandle((intptr_t)stdHandle, _O_TEXT);
            if (fileDescriptor != -1) {
                FILE* file = _fdopen(fileDescriptor, "w");
                if (file != NULL) {
                    int dup2Result = _dup2(_fileno(file), _fileno(stderr));
                    if (dup2Result == 0) {
                        setvbuf(stderr, NULL, _IONBF, 0);
                    }
                }
            }
        }
    }

    // Clear the error state for each of the C++ standard stream objects. We need to do this, as attempts to access the
    // standard streams before they refer to a valid target will cause the iostream objects to enter an error state. In
    // versions of Visual Studio after 2005, this seems to always occur during startup regardless of whether anything
    // has been read from or written to the targets or not.
    if (bindStdIn) {
        std::wcin.clear();
        std::cin.clear();
    }
    if (bindStdOut) {
        std::wcout.clear();
        std::cout.clear();
    }
    if (bindStdErr) {
        std::wcerr.clear();
        std::cerr.clear();
    }
}
}

void console_window::print_text(const char* text, int ansi_color) {
    previous_msg_.push_back(text);
    if (running_)
        role::Printf(ansi_color, "%s", text);
}

void console_window::run() {
    running_ = true;
    DWORD mode;
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    //std::ignore = _setmode(_fileno(stdin), _O_U16TEXT);
    if (owned_console_)
        for (const auto& i : previous_msg_) role::Printf(i.c_str());
    while (true) {
        std::string line;
        /*wchar_t wc;
        do {
            wc = _getwch();
            wline += wc;
            ungetwc(wc, stdin);
            std::wcout << wc;
        }
        while (wc != L'\n');
        std::cout << '\n';*/
        if (!bmmo::console::read_input(line)) {
            puts("stop");
            hide();
            break;
        };
        if (!running_)
            break;
        std::string cmd = "ballancemmo";
        std::vector<std::string> args;
        bmmo::command_parser parser(line);
        while (!cmd.empty()) {
            args.push_back(cmd);
            cmd = parser.get_next_word();
        }
        if (args.size() <= 1) continue;
        else if (args[1] == "mmo" || args[1] == "ballancemmo")
            args.erase(args.begin());
        log_manager_->get_logger()->Info("Execute command from console: %s", line.c_str());
        command_callback_(bml_, args);
    }
}

bool console_window::show() {
    bool console_allocated = AllocConsole();
    HWND hwnd = GetConsoleWindow();
    if (!hwnd)
        return false;
    owned_console_ = console_allocated;
    SetConsoleTitle("BallanceMMO Console");
    if (HMENU hMenu = GetSystemMenu(hwnd, FALSE)) {
        EnableMenuItem(hMenu, SC_CLOSE, MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
    }
    /*old_stdin = _dup(_fileno(stdin));
    old_stdout = _dup(_fileno(stdout));
    old_stderr = _dup(_fileno(stderr));*/
    BindCrtHandlesToStdHandles(true, true, true);
    /*        freopen("CONIN$", "r", stdin);
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);*/
    if (console_thread_.joinable())
        console_thread_.join();
    console_thread_ = std::thread([&]() { run(); });
    return true;
}

bool console_window::hide() {
    running_ = false;/*
    _dup2(old_stdin, _fileno(stdin));
    _dup2(old_stdout, _fileno(stdout));
    _dup2(old_stderr, _fileno(stderr));*/
    /*fclose(stdin);
    fclose(stdout);
    fclose(stderr);*/
    //std::this_thread::sleep_for(std::chrono::milliseconds(100));
    //TerminateThread(console_thread_.native_handle(), 0);
    if (!owned_console_)
        return false;
    owned_console_ = false;
    if (FreeConsole())
        return true;
    return false;
}

void console_window::free_thread() {
    if (console_thread_.joinable())
        console_thread_.join();
}
