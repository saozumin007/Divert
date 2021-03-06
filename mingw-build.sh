#!/bin/bash
#
# mingw-build.sh
# (C) 2013, all rights reserved,
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

# Script for MinGW/Linux cross compilation.
# NOTE: run wddk-build.bat before this script.

set -e

ENVS="i686-w64-mingw32 x86_64-w64-mingw32"

for ENV in $ENVS
do
    if [ $ENV = "i686-w64-mingw32" ]
    then
        CPU=i386
    else
        CPU=amd64
    fi
    if [ ! -d install/WDDK/$CPU ]
    then
        echo "WARNING: missing WDDK build; run wddk-build.bat first"
        echo "SKIP WDDK-$CPU"
        continue
    fi
    echo "BUILD WDDK-$CPU"
    CC="$ENV-gcc"
    STRIP="$ENV-strip"
    if [ -x "`which $CC`" ]
    then
        echo "\tmake install/MINGW/$CPU..."
        mkdir -p "install/MINGW/$CPU"
        echo "\tbuild install/MINGW/$CPU/WinDivert.dll..."
        $CC -Wall -O2 -Iinclude/ -c dll/windivert.c -o dll/windivert.o
        $CC -Wall -shared -o "install/MINGW/$CPU/WinDivert.dll" dll/windivert.o
        $STRIP --strip-debug "install/MINGW/$CPU/WinDivert.dll"
        echo "\tbuild install/MINGW/$CPU/netdump.exe..."
        $CC -s -O2 -Iinclude/ examples/netdump/netdump.c \
            -o "install/MINGW/$CPU/netdump.exe" -lWinDivert -lws2_32 \
            -L"install/MINGW/$CPU/"
        echo "\tbuild install/MINGW/$CPU/netfilter.exe..."
        $CC -s -O2 -Iinclude/ examples/netfilter/netfilter.c \
            -o "install/MINGW/$CPU/netfilter.exe" -lWinDivert -lws2_32 \
            -L"install/MINGW/$CPU/"
        echo "\tbuild install/MINGW/$CPU/passthru.exe..."
        $CC -s -O2 -Iinclude/ examples/passthru/passthru.c \
            -o "install/MINGW/$CPU/passthru.exe" -lWinDivert -lws2_32 \
            -L"install/MINGW/$CPU/"
        echo "\tbuild install/MINGW/$CPU/webfilter.exe..."
        $CC -s -O2 -Iinclude/ examples/webfilter/webfilter.c \
            -o "install/MINGW/$CPU/webfilter.exe" -lWinDivert -lws2_32 \
             -L"install/MINGW/$CPU/"
        echo "\tcopy install/MINGW/$CPU/WinDivert.inf..."
        cp install/WDDK/$CPU/WinDivert.inf install/MINGW/$CPU
        echo "\tcopy install/MINGW/$CPU/WinDivert.sys..."
        cp install/WDDK/$CPU/WinDivert.sys install/MINGW/$CPU
        echo "\tcopy install/MINGW/$CPU/WdfCoInstaller01009.dll..."
        cp install/WDDK/$CPU/WdfCoInstaller01009.dll install/MINGW/$CPU
    else
        echo "WARNING: $CC not found"
    fi
done

