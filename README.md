TODO: Don't forget licence stuff - add comment headers on files, a LICENCE file, something in README, etc

# basictool

## Table of contents
- [Overview](#overview)
- [How it works](#how-it-works)
- [Other features](#other-features)
- [Building](#building)
- [Credits](#credits)

## Overview

basictool is a command-line utility which can tokenise, de-tokenise, pack and analyse BBC BASIC programs for use with Acorn BBC BASIC on the BBC Micro and Acorn Electron.

BBC BASIC programs are tokenised by the interpreter when entered, and are usually saved and loaded in this tokenised form. This makes them hard to view or edit on a modern PC. basictool can tokenise and de-tokenise BBC BASIC programs:
```
$ # Start off with an ASCII text BASIC program, editable in any text editor.
$ cat test.bas
10PRINT "Hello, world!"
$ # Now tokenise it.
$ basictool -t test.bas test.tok
$ xxd test.tok
00000000: 0d00 0a15 f120 2248 656c 6c6f 2c20 776f  ..... "Hello, wo
00000010: 726c 6421 220d ff                        rld!"..
$ # Note that test.tok is tokenised - there's no "PRINT" in xxd's output.
$ # If you transferred test.tok to a BBC Micro, you could LOAD it in the usual way.
$ # Now we have a tokenised BASIC program, we can demonstrate de-tokenising it.
$ basictool test.tok
   10PRINT "Hello, world!"
$ # The previous command gave no output filename, so the output went to the terminal.
$ # But we can also de-tokenise to a file.
$ basictool test.tok test2.bas
$ # test2.bas is ASCII text BASIC, editable in any text editor.
$ cat test2.bas
   10PRINT "Hello, world!"
```

(basictool doesn't care about file extensions; I'm using .bas and .tok here for text BASIC and tokenised BASIC respectively, but you can use any conventions you wish.)

You can also "pack" a program, making it significantly less readable but more compact:
```
$ cat test3.bas
square_root_of_49 = 7
my_variable = 42 + square_root_of_49
PRINT my_variable
$ # Note that test3.bas doesn't have line numbers; basictool will add them for us.
$ basictool -p test3.bas
    1s=7:m=42+s:PRINTm
$ # basictool is happy to output the packed program as ASCII text, but it would
$ # usually make more sense to save it in tokenised form suitable for use with
$ # BBC BASIC.
$ basictool -p -t test3.bas test3.tok
$ xxd test3.tok
00000000: 0d00 0111 733d 373a 6d3d 3432 2b73 3af1  ...s=7:m=42+s:.
00000010: 6d0d ff                                  m..
```

By specifying the "-v" (verbose) option twice when packing, additional information will be displayed during the packing process:
```
$ basictool -p -v -v test3.bas
Info: Input auto-detected as ASCII text (non-tokenised) BASIC
TOP=&0E54
@% [0]
my_variable [2]                                   m
square_root_of_49 [2]                             s

TOP=&0E13
Bytes saved= 65
    1s=7:m=42+s:PRINTm
```

## How it works

basictool is really a specialised BBC Micro emulator built on top of lib6502. It runs the original BBC BASIC ROM and uses that to tokenise and de-tokenise programs. Programs are tokenised simply by typing them in at the BASIC prompt and de-tokenised simply by using the BASIC "LIST" command.

It also uses the Advanced BASIC Editor ROMs to provide the "pack" function, as well as to generate line number reference tables and variable cross-reference tables.

## Other features

### BASIC abbrevations

The standard BASIC abbreviations (such as "P." for "PRINT") can be used in text BASIC input to basictool.

### Renumbering

Programs can be renumbered using the --renumber option. You can control the start line number and gap between line numbers using --renumber-start and --renumber-step if you wish.

This has exactly the same restrictions as BASIC's "RENUMBER", because that's really what it is. In practice it works well, but if your program is calculating line numbers in a RESTORE statement or other advanced trickery, renumbering is likely to break it.

### Automatic line numbering

Line numbers are optional in text BASIC input; basictool will automatically generate them if they're not already present.

If you need to number some lines, such as DATA statements which will be used with RESTORE, you can give line numbers on just those lines:
```
$ cat test4.bas
PRINT "Hello, ";
RESTORE 1000
READ who$
PRINT who$;"!"
PROCend
END
DATA clouds, sky
1000DATA world
DEF PROCend
PRINT "Goodbye!
ENDPROC
$ basictool test4.bas
    1PRINT "Hello, ";
    2RESTORE 1000
    3READ who$
    4PRINT who$;"!"
    5PROCend
    6END
    7DATA clouds, sky
 1000DATA world
 1001DEF PROCend
 1002PRINT "Goodbye!
 1003ENDPROC
```
This will fail if the line numbers you provide would clash with the automatically generated line numbers; I suggest using large round values in order to avoid this being a problem.

If you don't like the line numbers generated by the automatic line numbering, you can use the --renumber option to tidy things up.

(Automatic line numbering works exactly the same as in [beebasm](https://github.com/stardot/beebasm)'s PUTBASIC command, if you're already familiar with that.)

### Formatted output

Two different styles of "pretty-printed" formatted output are available.

I'll use this simple program to demonstrate:
```
$ cat test5.bas
FOR I=1 TO 100
FOR J=1 TO 100
PRINT I*J:IF I+J>42 THEN PRINT "Foo!":FOR K=1 TO 100:NEXT
NEXT
NEXT
```

The first option is BASIC's own LISTO command. LISTO 7 is probably the most useful here:
```
$ basictool --listo 7 test5.bas
    1 FOR I=1 TO 100
    2   FOR J=1 TO 100
    3   PRINT I*J:IF I+J>42 THEN PRINT "Foo!":FOR K=1 TO 100:NEXT
    4   NEXT
    5 NEXT
```

The second option is the Advanced BASIC Editor's "format" utility:
```
$ basictool -f test5.bas
    1 FOR I=1 TO 100
    2   FOR J=1 TO 100
    3     PRINT I*J:
          IF I+J>42
            THEN PRINT "Foo!":
            FOR K=1 TO 100:
            NEXT
    4   NEXT
    5 NEXT
```

### Analysing a program

You can generate line number reference tables and variable cross-reference tables using the Advanced BASIC Editor's utilities. Obviously these are more useful with larger programs, but here's a simple demonstration:
```
$ cat test6.bas
10PRINT "Hello, ";:who$="world":end$="Goodbye!"
20GOTO 50
30PRINT "!"'end$
40END
50PRINT ;who$;
60GOTO 30
$ basictool --line-ref test6.bas
   30    (   60)
   50    (   20)
$ basictool --variable-xref test6.bas
@% [0]
end$ [2]                                          10    30
who$ [2]                                          10    50
$
```

### Other options

There are a few options not described here which just provide ways to tweak basictool's behaviour. Use:
```
$ basictool --help
```
to see all the options.

## Building

I developed basictool on Linux, but the code does not use any platform-specific features and should build with few or no changes on any operating system with a C99 compiler.

## Downloading the basictool code

The simplest option is to download a release zip file from the [releases](https://github.com/ZornsLemma/basictool/releases) pages; it's probably best to pick the most recent. Unzip that somewhere using your favourite tool. If you prefer, you can "git clone" this repository and check out the branch or tag of interest.

### Getting the ROM images

You need to download three ROM images and put then in the roms subdirectory:

* EDITORA100 and EDITORB100 from https://stardot.org.uk/forums/viewtopic.php?p=81362#p81362
* BBC BASIC 4r32 from http://mdfs.net/Software/BBCBasic/BBC/

Once you've done that, the roms subdirectory should look like this:
```
$ ls -l
total 52
-rw-rw-r-- 1 steven steven 16384 Apr  3 14:26 Basic432
-rw-rw-r-- 1 steven steven 16384 Apr  3 14:26 EDITORA100
-rw-rw-r-- 1 steven steven 16384 Apr  3 14:26 EDITORB100
-rw-rw-r-- 1 steven steven   177 Apr  3 14:35 README.md
$ md5sum *
e11eed95d1caba8aa9772e9001590585  Basic432
101fb6907609918db50cb3cfd5408772  EDITORA100
987a8aa4e09880e4274ddc26025b06e8  EDITORB100
```

Other versions of 6502 BBC BASIC (although not HIBASIC) will probably work, but I've always tested with BASIC 4r32 so that's what I recommend you use.

The ROMs are compiled into the executable, so these files are only need when building - the resulting executable is self-contained and can be copied to wherever you want to install it.

### Compiling

If you're on a Unix-like system, you should just be able to do:
```
$ cd src
$ make
```
This will create a basictool executable in the top-level project directory, i.e. the one above src.

The executable has the ROM images compiled into it, so it can be copied wherever you want to install it and it should just work.

If you're not on a Unix-like system you may need to create your own build script or project file for your IDE. src/Makefile may be a good starting point; alternatively there is a simple shell script src/make.sh which will perform a build and it may be easiest for you to translate that into your platform's equivalent of a shell script. If you do this, please consider sending me the results so I can add them and make it easier for other people to build on your platform.

## Credits and thanks

* BBC BASIC was (of course) originally published by Acorn. The BASIC 4r32 ROM image linked above is hosted on J. G. Harston's [mdfs.net](http://mdfs.net/).

* The BASIC editor and utilities were originally published separately by Altra. The Advanced BASIC Editor ROMs used here are (C) Baildon Electronics. The Advanced BASIC Editor ROM images linked above are hosted on [stardot](https://stardot.org.uk) and were posted by J. G. Harston. Thanks to Dave Hitchens for his support for developing basictool using these ROMs.

* 6502 emulation is performed using lib6502. This was originally written by Ian Piumarta, but the versions of lib6502.[ch] included here are taken from [PiTubeDirect](https://github.com/hoglet67/PiTubeClient).

* Cross-platform command line parsing (cargs.[ch]) is performed using [cargs](https://github.com/likle/cargs).
