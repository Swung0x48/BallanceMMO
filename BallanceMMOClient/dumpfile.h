#pragma once
#include <functional>


namespace NSDumpFile
{
    constexpr const char* DumpPath = "..\\CrashDumps";

    constexpr const char* Quotes[] = {
         "Look mom I'm on this quote!",
         "Uh-oh.",
         "Ouch!",
         "Noooooooooooooooo",
         "Help! HELP!",
         "Something went wrong...",
         "To run or not to run, that is the question.",
         "Task failed successfully.",
         "It's all my fault.",
         "Gonna give you up and let you down",
         "You know the rules and so do I.",
         "The game is a lie!",
         "ALL YOUR GAME ARE BELONG TO ERRORS",
         "Wow I can has faytle errerz?",
         "Error displaying error quotes.",
         "F",
         "Meow :3",
         "FATAW EWWA",
         "Error details:\n- Error displaying the previous error.",
         "Error messages\ncannot completely convey.\nWe now know shared loss.",
         "Fun fact: MMO stands for Massive Massacre On-going",
         "A wild exception appeared!\nException used Fatal Error!\nIt's super effective!",
    };

    void RunCrashHandler(std::function<void(char*)> Callback);

    // Hang watch (2026-11-08 public test: three client freezes, each ended
    // from the Task Manager, and nothing on disk said where the game thread
    // was).  The game thread beats once a rendered frame; while it is armed
    // (a physics session runs), no beat comes for kHangMs and Windows calls
    // the game window hung (IsHungAppWindow: no message pumped for 5 s), a
    // watch thread writes a dump of every thread's stack,
    // BMMO_<version>_<time>_hang.dmp next to the crash dumps - once a stall.
    // The beat that ends a stall with a dump returns its report.
    constexpr unsigned kHangMs = 5000;
    void StartHangWatch();
    void StopHangWatch();
    // Game thread.  Returns true and fills `report` when the stall this beat
    // ends wrote a dump.
    bool HangWatchBeat(bool armed, char* report, size_t size);
};


#define DeclareDumpFile(Callback) NSDumpFile::RunCrashHandler(Callback);