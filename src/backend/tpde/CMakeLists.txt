# bober's back end of TPDE behind the C API of tpde.h, with TPDE's headers found where bober.pkg installed them.
cmake_minimum_required(VERSION 3.20)
project(bober-tpde CXX)
find_path(TPDE tpde/CompilerBase.hpp REQUIRED)
add_library(bober-tpde arm64.cpp ir.cpp x64.cpp)
target_include_directories(bober-tpde PRIVATE ${TPDE})
target_compile_features(bober-tpde PRIVATE cxx_std_20)
install(TARGETS bober-tpde)
