# sharkmidi

A (currently incomplete) C port of [SharpMIDI-Raylib](https://github.com/imnotacoderlmao/SharpMIDI-raylib) to leverage a smarter compiler (totally not a very similar case that laid [ConMIDI](https://github.com/EmK530/ConMIDI) noooooo!!!!!)

## Quick info

pain and suffering - Copy

## Requirements

midi files i think

and a synth that supports [KDMAPI](https://github.com/KeppySoftware/OmniMIDI/blob/master/DeveloperContent/KDMAPI.md)

## Build Info

Install a C compiler and [make](https://www.gnu.org/software/make/) if youre on linux. otherwise, a MinGW wrapper since im too lazy to maintain two completely different codebases ([w64devkit](https://github.com/skeeto/w64devkit) should be good for it) then run 'make' on the root dir.
there is several arguments like multiprocessing for faster parsing and cpu specific compilation if you want 2 less clock cycles per function.

if you are on windows, path to a GLFW installation folder is required for the visualizer to get headers

## Quirks

i need to memory safely

you *might* need to change some things in the source code or makefile if on windows, i dont have a native machine so i have no idea if it works flawlessly there or not

## Credits

original SharpMIDI: EmK530

maybe chatgpt and claude cause my brain still dosent work sometimes :sob:

## License

[GNU GPLv3](https://choosealicense.com/licenses/gpl-3.0/)
