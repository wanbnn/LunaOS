cmake_minimum_required(VERSION 3.18)
project(LunaSDKNativeDriver LANGUAGES C)

# Host cross-build only. CMake and a dynamic ELF loader are NOT in the guest yet.
# Compile a native kernel driver object using the same freestanding i386 ABI.
add_library(lunasdk_edu_example OBJECT ${CMAKE_CURRENT_LIST_DIR}/../../kernel/lunasdk_edu.c)
target_include_directories(lunasdk_edu_example PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../kernel)
target_compile_features(lunasdk_edu_example PRIVATE c_std_11)
target_compile_options(lunasdk_edu_example PRIVATE
  -m32 -O2 -ffreestanding -fno-builtin -fno-pie -fno-pic
  -fno-stack-protector -fno-asynchronous-unwind-tables
  -mno-sse -mno-mmx -msoft-float -Wall -Wextra -Werror)
