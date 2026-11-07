# LLD behind the C API of lld.h, with LLD found where bober.pkg installed it.
cmake_minimum_required(VERSION 3.20)
project(bober-lld CXX)
find_package(LLD REQUIRED CONFIG)
add_library(bober-lld lld.cpp)
target_include_directories(bober-lld PRIVATE ${LLD_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
target_compile_features(bober-lld PRIVATE cxx_std_20)
install(TARGETS bober-lld)
