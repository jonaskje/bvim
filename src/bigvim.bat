:: command to build big Vim with Python for Windows 7
call "%VS120COMNTOOLS%vsvars32.bat"
set SDK_INCLUDE_DIR=%ProgramFiles(x86)%\Microsoft SDKs\Windows\v7.1A\Include
set VIM_ADD_FEATURES=FEATURES=BIG GUI=yes PYTHON=C:\python27 DYNAMIC_PYTHON=yes PYTHON_VER=27
set VIM_REMOVE_FEATURES=CSCOPE=no NETBEANS=no XPM=no
set VIM_FEATURES=%VIM_ADD_FEATURES% %VIM_REMOVE_FEATURES%
nmake -f Make_mvc.mak WINVER=0x0601 %VIM_FEATURES%
pause
