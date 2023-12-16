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
};


#define DeclareDumpFile(Callback) NSDumpFile::RunCrashHandler(Callback);