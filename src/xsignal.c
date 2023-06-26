/****************************************************************************
 * xsignal.c
 * openacousticdevices.info
 * March 2023
 *****************************************************************************/

#include <stdbool.h>

#include "xsignal.h"

#if defined(_WIN32) || defined(_WIN64)

    #include <windows.h>

    BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {

         Signal_handleSignal();

         return true;

    }

    void Signal_registerHandler() {

        SetConsoleCtrlHandler(CtrlHandler, true);

    }

#else

    #include <signal.h>

    void signalHandler(int dummy) {

        Signal_handleSignal();

    }

    void Signal_registerHandler(void) {

        signal(SIGHUP, signalHandler);

        signal(SIGINT, signalHandler);

        signal(SIGQUIT, signalHandler);

        signal(SIGTERM, signalHandler);

        signal(SIGTSTP, signalHandler);

    }

#endif
