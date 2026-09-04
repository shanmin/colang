#
# CMakeLists.txt — Co 语言编译器构建配置
#
# 依赖：LLVM 开发库（C++ 头文件 + 静态库 + CMake 配置文件）
#   注意：官方 "C:\Program Files\LLVM" 工具链安装（clang 17.0.6）不含开发库，
#   不能用于编译本项目。需要 vcpkg、自行编译或另行获取的完整 LLVM。
#
# 常用命令（VS 自带 CMake，不在 PATH，用完整路径或先配置环境）：
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#
# 如果 LLVM 不在默认查找路径，需指定配置目录：
#   cmake -B build -DLLVM_DIR="<LLVM根目录>/lib/cmake/llvm"
#
# 如果使用 vcpkg 的 llvm：
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg根目录>/scripts/buildsystems/vcpkg.cmake
#

cmake_minimum_required(VERSION 3.20)

project(colang LANGUAGES CXX)

# C++ 标准（PROJECT.md 技术路线约定 C++20）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 运行时库选择：
#   OFF = /MD 动态 CRT（默认）：必须与 LLVM 静态库的 CRT 一致。
#         本地 LLVM 23.1.0 构建树默认用动态 CRT 编译（其 obj 内为 __imp_ 导入引用），
#         colang 若用 /MT 链接会出现 19 个 LNK2019 无法解析符号（__imp_rand 等）
#   ON  = /MT 静态 CRT：仅在以 LLVM_USE_CRT=MT 重新编译 LLVM 后才可用
option(COLANG_STATIC_CRT "Use static CRT (/MT), must match how LLVM libs were built" OFF)

# 查找 LLVM（C++ API 开发库）
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# LLVM 头文件与编译定义
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})

# 源码收集（每个 AST 类一个 cpp，新增文件自动纳入）
file(GLOB COLANG_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_executable(colang ${COLANG_SOURCES})

target_include_directories(colang PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# LLVM 组件：
#   Core       — IRBuilder / Module / Function / Verifier 等
#   Support    — raw_fd_ostream / raw_string_ostream 等
#   BitWriter  — WriteBitcodeToFile（输出 .bc 文件）
#   BitReader  — parseBitcodeFile（AST_import 从 .bc 读回导入模块做声明注入）
llvm_map_components_to_libnames(COLANG_LLVM_LIBS Core Support BitWriter BitReader)
target_link_libraries(colang PRIVATE ${COLANG_LLVM_LIBS})

# MSVC 专属设置
if(MSVC)
  # /utf-8：源文件为 UTF-8 无 BOM，中文注释与字符串字面量必须显式声明编码，
  #         否则 MSVC 按系统代码页（GBK）解释，产生乱码或编译错误
  # /Zc:__cplusplus：正确上报 __cplusplus 宏（C++20 特性检测需要）
  target_compile_options(colang PRIVATE /utf-8 /Zc:__cplusplus /W3)

  if(COLANG_STATIC_CRT)
    set_property(TARGET colang PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

#	THE END
