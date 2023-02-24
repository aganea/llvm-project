#!/usr/bin/bash

TARGETDIR=$1
echo $TARGETDIR
rm -rf $TARGETDIR
mkdir $TARGETDIR
cd $TARGETDIR

OPT_BASE=-march=native
OPT_OPT='-march=native -Xclang -O3 -fstrict-aliasing -flto=thin -fwhole-program-vtables'
STAGE1=stage1_linux

if [ $TARGETDIR == 'stage1_linux' ]; then
	cmake -GNinja ../llvm -DCMAKE_BUILD_TYPE=Release -DLLVM_OPTIMIZED_TABLEGEN=ON -DCMAKE_C_COMPILER=clang-15 -DCMAKE_CXX_COMPILER=clang++-15 -DCMAKE_LINKER=ld.lld-15 -DLLVM_ENABLE_LLD=ON -DLLVM_ENABLE_PROJECTS="llvm;lld;clang" -DCMAKE_CXX_FLAGS='$OPT_BASE' -DCMAKE_C_FLAGS='$OPT_BASE'
fi

if [ $TARGETDIR == 'stage2_linux' ]; then
	cmake -GNinja ../llvm -DCMAKE_BUILD_TYPE=Release -DLLVM_OPTIMIZED_TABLEGEN=ON -DCMAKE_C_COMPILER='$STAGE1/bin/clang-15' -DCMAKE_CXX_COMPILER='$STAGE1/bin/clang++-15' -DCMAKE_LINKER='$STAGE1/bin/ld.lld-15' -DLLVM_ENABLE_LLD=ON -DLLVM_ENABLE_PROJECTS="llvm;lld;clang" -DCMAKE_CXX_FLAGS='$OPT_OPT -fuse-ld=lld' -DCMAKE_C_FLAGS='$OPT_OPT -fuse-ld=lld' -DLLVM_ENABLE_LTO=Thin -DLLVM_ENABLE_RPMALLOC=ON
fi
