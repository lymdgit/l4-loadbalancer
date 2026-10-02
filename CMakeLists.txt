cmake_minimum_required(VERSION 3.16)
project(l4-loadbalancer VERSION 2.0.0 LANGUAGES C CXX)

# ============================================================================
# 基于纯 DPDK 的四层负载均衡器
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(L4LB_NATIVE    "Optimize for the build machine (-march=native)" ON)
option(L4LB_SANITIZE  "Build with AddressSanitizer + UBSan (Debug use)" OFF)
option(L4LB_HW_CKSUM  "Use NIC TX checksum offload when available" ON)
option(BUILD_TESTS    "Build unit tests (ctest)" ON)

# ============================================================================
# DPDK（通过 pkg-config 查找；DPDK 不在默认路径时设置 PKG_CONFIG_PATH）
# ============================================================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK IMPORTED_TARGET libdpdk)
if(NOT DPDK_FOUND)
    message(FATAL_ERROR
        "libdpdk not found via pkg-config. Set PKG_CONFIG_PATH, e.g.\n"
        "  PKG_CONFIG_PATH=<dpdk-install>/lib64/pkgconfig cmake ..")
endif()

# ============================================================================
# 编译选项
# ============================================================================
add_compile_options(-Wall -Wextra -fno-omit-frame-pointer)
if(L4LB_NATIVE)
    add_compile_options(-march=native)
endif()
if(L4LB_SANITIZE)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()
add_compile_definitions(ALLOW_EXPERIMENTAL_API _GNU_SOURCE)
if(L4LB_HW_CKSUM)
    add_compile_definitions(L4LB_HW_CKSUM)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT L4LB_IPO_OK OUTPUT L4LB_IPO_MSG LANGUAGES CXX)

# ============================================================================
# 源文件：src/ 与 include/ 一一对应；main.cpp 单独作为可执行文件入口
# ============================================================================
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")

add_library(l4lb_core STATIC ${CORE_SOURCES})
target_include_directories(l4lb_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(l4lb_core PUBLIC PkgConfig::DPDK pthread)

add_executable(l4lb src/main.cpp)
target_link_libraries(l4lb PRIVATE l4lb_core)

if(L4LB_IPO_OK AND NOT L4LB_SANITIZE)
    # 拆分为多个 .cpp 后，开启 LTO 让编译器仍能跨文件内联热路径函数
    set_property(TARGET l4lb_core l4lb PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# ============================================================================
# 单元测试：ctest 运行，需要 hugepage 的用例使用 --no-huge 启动 EAL
# ============================================================================
if(BUILD_TESTS)
    enable_testing()
    file(GLOB UNIT_TESTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/unit/test_*.cpp")
    foreach(src ${UNIT_TESTS})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE l4lb_core)
        target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()

# ============================================================================
# 安装
# ============================================================================
install(TARGETS l4lb RUNTIME DESTINATION bin)
install(FILES config/lb.conf config/lb_dr.conf DESTINATION etc/l4lb)
install(PROGRAMS scripts/setup_dpdk_env.sh scripts/l4lbctl.py DESTINATION bin)

message(STATUS "l4lb: build type=${CMAKE_BUILD_TYPE} native=${L4LB_NATIVE} "
               "sanitize=${L4LB_SANITIZE} lto=${L4LB_IPO_OK} tests=${BUILD_TESTS}")
