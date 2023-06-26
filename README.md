# AudioMoth-Live #

This is a command line tool for recording live audio from high sample rate microphones, including the AudioMoth USB Microphone.

## How to use it ##

The AudioMoth-Live command line tool is described in detail in the Application Note [here](https://github.com/OpenAcousticDevices/Application-Notes/blob/master/Using_the_AudioMoth_Live_App_with_the_AudioMoth_USB_Microphone/Using_the_AudioMoth_Live_App_with_the_AudioMoth_USB_Microphone.pdf).

The following command will write one-minute files at 48kHz to the local directory `files` using either the default input or an AudioMoth USB Microphone (if connected).

```
> AudioMoth-Live autosave 1 files
```

The following command does the same on Windows.

```
> AudioMoth-Live.exe autosave 1 files
```

## Building ##

AudioMoth-Live can be built on macOS using the Xcode Command Line Tools.

```
> clang -I../inc/ -I../miniaudio/ -framework CoreAudio ../src/*.c -o AudioMoth-Live 
```

AudioMoth-Live can be built on Windows using the Microsoft Visual C++ Build Tools. Note that to build the correct version you should run the command in the correct environment. Use the 'x64 Native Tools Command Prompt' to build the 64-bit binary on a 64-bit machine, and the 'x64_x86 Cross Tools Command Prompt' to build the 32-bit binary on a 64-bit machine.

```
cl /I..\inc\ /I..\miniaudio\ ..\src\*.c /link /out:.\dist\AudioMoth-Live.exe
```

AudioMoth-Live can be built on Linux and Raspberry Pi using the `gcc`.

```
gcc -I./inc/ -I./miniaudio/ ./src/*.c -o AudioMoth-Live -ldl -lpthread -lm -latomic
```

On macOS, Linux and Raspberry Pi you can copy the resulting executable to `/usr/local/bin/` so it is immediately accessible from the terminal. On Windows copy the executable to a permanent location and add this location to the path.
