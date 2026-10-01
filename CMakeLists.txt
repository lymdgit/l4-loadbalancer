cmake_minimum_required(VERSION 3.16)
project(l4-loadbalancer VERSION 1.0.0 LANGUAGES C CXX)

# ============================================================================
# 项目说明：基于纯 DPDK 的四层负载均衡器（不依赖 F-Stack）
# 优势：
# - 没有 F-Stack TCP 栈干扰，不会发送不必要的 RST
# - 完全控制数据包流程
# - 更简单、更高效
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# DPDK 配置
# ============================================================================
# 优先使用 pkg-config 查找 DPDK
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(DPDK IMPORTED_TARGET libdpdk)
endif()

# 如果 pkg-config 找不到，使用手动配置
if(NOT DPDK_FOUND)
    set(DPDK_PATH "/data/f-stack/dpdk" CACHE PATH "DPDK installation path")
    set(DPDK_LIB_PATH "${DPDK_PATH}/build/lib")
    set(DPDK_INCLUDE_PATH "${DPDK_PATH}/build/include")
    
    # 备用路径
    if(NOT EXISTS "${DPDK_INCLUDE_PATH}")
        set(DPDK_INCLUDE_PATH "${DPDK_PATH}/include")
    endif()
    
    message(STATUS "Using manual DPDK path: ${DPDK_PATH}")
    message(STATUS "DPDK include: ${DPDK_INCLUDE_PATH}")
    message(STATUS "DPDK lib: ${DPDK_LIB_PATH}")
endif()

# ============================================================================
# 编译选项
# ============================================================================
add_compile_options(
    -Wall                   # 开启所有警告
    -Wextra                 # 开启额外警告
    -O3                     # 高优化级别（压测场景）
    -march=native           # 针对本机 CPU 优化
    -g                      # 调试信息（perf 将地址映射回函数名）
    -fno-omit-frame-pointer # 保留帧指针，perf 回溯调用栈必须（火焰图）
)

# DPDK 需要的宏定义
add_definitions(
    -DALLOW_EXPERIMENTAL_API
    -D_GNU_SOURCE
    -DL4LB_HW_CKSUM
)

# ============================================================================
# 包含目录
# ============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

if(DPDK_FOUND)
    include_directories(${DPDK_INCLUDE_DIRS})
else()
    include_directories(${DPDK_INCLUDE_PATH})
endif()

# ============================================================================
# 源文件收集
# ============================================================================
# src/ 与 include/ 目录一一对应：
#   common/    配置、日志、公共类型
#   protocol/  协议头、解析、校验和、ARP/ICMP
#   lb/        会话表、RS 管理、一致性哈希
#   forward/   NAT / DR 转发引擎
#   core/      LoadBalancer 报文分发
#   dataplane/ 端口初始化、worker 循环、数据面全局状态
# CONFIGURE_DEPENDS：新增 .cpp 后重新构建即可被收集，无需手动重跑 cmake
file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

# ============================================================================
# 主程序目标
# ============================================================================
add_executable(l4lb ${ALL_SOURCES})

# 拆分为多个 .cpp 后，开启 LTO 让编译器仍能跨文件内联热路径函数
include(CheckIPOSupported)
check_ipo_supported(RESULT L4LB_IPO_OK OUTPUT L4LB_IPO_MSG LANGUAGES CXX)
if(L4LB_IPO_OK)
    set_property(TARGET l4lb PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    message(STATUS "LTO: enabled")
else()
    message(STATUS "LTO: not supported (${L4LB_IPO_MSG})")
endif()

# ============================================================================
# 链接 DPDK 库（纯 DPDK，不依赖 F-Stack）
# ============================================================================

if(DPDK_FOUND)
    # 使用 pkg-config 找到的配置
    # 导入目标自带头文件路径、库路径和编译选项
    target_link_libraries(l4lb PkgConfig::DPDK)
else()
    # 手动链接 DPDK 库
    link_directories(${DPDK_LIB_PATH})
    
    # DPDK 核心库列表（按依赖顺序）
    set(DPDK_LIBS
        # 使用 whole-archive 确保所有符号被包含
        -Wl,--whole-archive
        rte_eal
        rte_log
        rte_mempool
        rte_mempool_ring
        rte_ring
        rte_mbuf
        rte_ethdev
        rte_net
        rte_kvargs
        rte_hash
        rte_rcu
        rte_timer
        rte_cmdline
        rte_telemetry
        rte_pci
        rte_bus_pci
        rte_bus_vdev
        rte_net_vmxnet3    # VMware 网卡驱动
        rte_net_e1000      # Intel 网卡驱动
        rte_net_virtio     # Virtio 网卡驱动
        -Wl,--no-whole-archive
    )
    
    target_link_libraries(l4lb ${DPDK_LIBS})
    target_include_directories(l4lb PRIVATE ${DPDK_INCLUDE_PATH})
endif()

# 通用依赖库
target_link_libraries(l4lb
    pthread
    dl
    numa
    m
)

# ============================================================================
# 测试配置 (可选)
# ============================================================================
option(BUILD_TESTS "Build unit tests" OFF)

if(BUILD_TESTS)
    enable_testing()
    
    # 下载 Google Test
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
    
    # 添加测试可执行文件
    add_executable(test_consistent_hash tests/unit/test_consistent_hash.cpp)
    target_link_libraries(test_consistent_hash GTest::gtest_main)
    target_include_directories(test_consistent_hash PRIVATE ${CMAKE_SOURCE_DIR}/include)
    
    add_executable(test_ring_buffer tests/unit/test_ring_buffer.cpp)
    target_link_libraries(test_ring_buffer GTest::gtest_main pthread)
    target_include_directories(test_ring_buffer PRIVATE ${CMAKE_SOURCE_DIR}/include)
    
    add_executable(test_protocol tests/unit/test_protocol.cpp)
    target_link_libraries(test_protocol GTest::gtest_main)
    target_include_directories(test_protocol PRIVATE ${CMAKE_SOURCE_DIR}/include)
    
    include(GoogleTest)
    gtest_discover_tests(test_consistent_hash)
    gtest_discover_tests(test_ring_buffer)
    gtest_discover_tests(test_protocol)
endif()

# ============================================================================
# 安装配置
# ============================================================================
install(TARGETS l4lb RUNTIME DESTINATION bin)
install(FILES config/lb.conf DESTINATION etc/l4lb)

# ============================================================================
# 输出配置信息
# ============================================================================
message(STATUS "====================================")
message(STATUS "L4 Load Balancer Configuration:")
message(STATUS "  Mode: Pure DPDK (No F-Stack)")
if(DPDK_FOUND)
    message(STATUS "  DPDK: Found via pkg-config")
else()
    message(STATUS "  DPDK Path: ${DPDK_PATH}")
endif()
message(STATUS "  Build Tests: ${BUILD_TESTS}")
message(STATUS "====================================")
