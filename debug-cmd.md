D:/git/llvm/llvm-playground/.build-2019-x64/sample/Debug/sample.exe

lldb -n sample.exe

process attach --name sample.exe

breakpoint set -f main.cpp -l 54

thread backtrace

thread step-out

thread list

thread select 1



