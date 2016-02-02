BVIM, gvim with Visual Studio extensions
=======================================================

bvim is a hacked version of gvim which adds a few features which helps working on large Visual Studio projects.

The goal is to make all common programming actions take less than 500ms on a fast machine.

Bvim is supported on Windows 7 and builds with VS2013.

Build bvim using src/vim_vs2013.sln with Python 2.7 installed at C:\Python27.

boresln `<visual studio .sln file`>
------------------------------------------------------
Open a solution and build a list of all files that are included in the projects. This must be the first thing done in order to use the other commands.

boreopen
-------------------------------------------------------
Open a help-like window listing all files in the solution. Use / to search for the wanted file and press Enter to open it.

boretoggle
-------------------------------------------------------
Cycle between related files in the solution. The order is hardcoded to> cpp cxx c inl hpp hxx h asm s ddf

borefind [-e ext1,ext2,...,ext12] `<string`>
-------------------------------------------------------
Do a case sensitive search through all files in the solution for <string>, optionally limited to a set of file extensions. At most 100 hits per file is reported and the total hits is capped to 1000. 

g:bore_base_dir
-------------------------------------------------------
The base directory of the solution file. It is either the directory of the solution file itself, or its parent directory. All bore file paths are relative to this directory. Useful for e.g. writing a single tags file from all solution files.

g:bore_filelist_file
-------------------------------------------------------
The filename of the file which contains a list of relative paths for all files included in the solution. Useful for e.g. building a tags file from all solution files.

g:bore_search_thread_count
-------------------------------------------------------
The number of threads used by borefind. Defaults to 4.
