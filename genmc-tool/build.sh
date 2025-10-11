#!/bin/bash

cd ~/lib/
wget https://github.com/llvm/llvm-project/releases/download/llvmorg-16.0.0/llvm-project-16.0.0.src.tar.xz
tar xf llvm-project-16.0.0.src.tar.xz
cd llvm-project-16.0.0.src
mkdir -p build && cd build


cmake -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS="clang;lld" \
      -DLLVM_ENABLE_RTTI=ON \
      -DCMAKE_INSTALL_PREFIX=/usr/local/llvm-16 \
      ../llvm
make -j$(nproc)
sudo make install

echo 'export PATH=/usr/local/llvm-16/bin:$PATH' >> ~/.zshrc
source ~/.zshrc

rm -rf autom4te.cache
rm configure Makefile.in aclocal.m4
autoreconf --install
./configure \
    --with-clang=$(which clang-16) \
    --with-clangxx=$(which clang++-16) \
    --with-llvm=$(llvm-config-19 --prefix)

make  -j ${nproc}
./genmc --version

