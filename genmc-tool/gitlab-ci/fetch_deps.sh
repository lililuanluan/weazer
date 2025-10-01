#!/bin/bash

# Fetches all necessary dependencies for GenMC's docker image.
#
# Depends: libtinfo5
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, you can access it online at
# http://www.gnu.org/licenses/gpl-3.0.html.
#
# Author: Michalis Kokologiannakis <michalis@mpi-sws.org>

# error handling
err() {
  echo "[$(date +'%Y-%m-%dT%H:%M:%S%z')]: $*" >&2
}

# install dependencies
triple=x86_64-linux-gnu
os=ubuntu
tar=tar.xz

mkdir -p cache

case $LLVM_VERSION in
    3.[5].*)
	ubuntu_ver=14.04
	;;
    3.[8]*|[4-6].*)
        ubuntu_ver=16.04
        ;;
    ?*)
        ubuntu_ver=18.04
        ;;
esac
case $LLVM_VERSION in
    *-rc*|1?.*)
        url_prefix="https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VERSION"
        ;;
    *)
        url_prefix="http://releases.llvm.org/$LLVM_VERSION"
        ;;
esac
url="$url_prefix/clang+llvm-$LLVM_VERSION-$triple-$os-$ubuntu_ver.$tar"
llvm_tar="cache/$LLVM_VERSION.$tar"
llvm_bin="cache/clang+llvm-$LLVM_VERSION"

# download
echo -ne "Downloading LLVM/clang-$LLVM_VERSION... "
if [[ ! -f $llvm_tar ]]; then
    wget $url -o wget-$LLVM_VERSION.log -O $llvm_tar
    if [ ! $? -eq 0 ]; then
	err "Error: Download failed"
	rm $llvm_tar || true
	exit 1
    fi
    echo "Done"
else
    echo "Found Cached Archive"
fi

# install
echo -ne "Extracting LLVM... "
if [[ ! -d $llvm_bin ]]; then
    tar -xf $llvm_tar -C cache/
    if [[ ! $? -eq 0 ]]; then
	err "Error: Extraction Failed"
	rm $llvm_tar || true
	exit 1
    fi
    case $LLVM_VERSION in
        3.5.*)
	    mv cache/clang+llvm-$LLVM_VERSION-$triple cache/clang+llvm-$LLVM_VERSION
	    ;;
        ?*)
            mv cache/clang+llvm-$LLVM_VERSION-$triple-$os-$ubuntu_ver cache/clang+llvm-$LLVM_VERSION
	    ;;
    esac
    echo "Done"
else
    echo "Found Cached Installation"
fi
