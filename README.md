# lsct
A Linux command line tool for recursively listing files in a directory, sorted by content-type. 

#### Motivation
The idea of the tool stems from the fact that compressed ```tar``` archives get smaller in size if the files are sorted by their type before the compression. The first attempt to sort files by content-type using ```file``` and ```sort``` utilities failed because on my Linux Mint 17 the ```file``` command cannot accept or produce null-terminanated strings and ```sort``` cannot select the part of the string after the last delimiter.

#### Usage
By default the tool recursively lists files and symbolic links only, ignoring all entries starting from "." and not following the links. The behaviour can be adjusted by supplying any of the following options:
* ```-a, --all```   do not ignore entries starting from "."
* ```-m, --mime```  output using the format: ```<comtent-type>: <file>```
* ```-0, --null```  use null instead of new-line to separate output lines.

The output of the utility can be, for example, piped into a ```tar``` command for further processing. 

#### Implementation
The implementation is build around ```nftw(3)``` function, filling in a binary tree with the entry names and then traversing the tree. The tool relies on ```libmagic``` for providing the content-type information.

#### Compilation
There are two simple shell scripts, ```build-debug``` and ```build-release```, to compile for debug and release respectively. The software has been compiled and tested on Linux Mint 17:
```bash
$ uname -sri
Linux 3.13.0-37-generic i686
$ gcc --version
gcc (Ubuntu 4.8.2-19ubuntu1) 4.8.2
```

#### Limitations
So far it has been found that sometimes ```libmagic``` reports some ```.mp3``` files as being of type ```application/octet-stream``` instead of ```audio/mpeg```, though those files can be played by vlc and other players with no problem at all.

###### Licence: BSD
