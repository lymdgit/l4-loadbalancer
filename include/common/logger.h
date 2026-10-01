/**
 * @file logger.h
 * @brief 高性能日志系统
 * 
 * 设计目标：
 * 1. 低开销：条件编译控制调试日志
 * 2. 线程安全：使用互斥锁保护输出
 * 3. 灵活配置：支持多种日志级别
 * 4. 格式化输出：时间戳、级别、位置信息
 * 
 * 使用方式：
 * LOG_INFO("Connection from %s:%d", ip, port);
 * LOG_DEBUG("Packet received, len=%zu", len);
 * LOG_ERROR("Failed to allocate memory");
 * 
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_COMMON_LOGGER_H
#define L4LB_COMMON_LOGGER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace l4lb {

/**
 * @brief 日志级别枚举
 */
enum class LogLevel : int {
    DEBUG = 0,      ///< 调试信息，最详细
    INFO  = 1,      ///< 普通信息
    WARN  = 2,      ///< 警告信息
    ERROR = 3,      ///< 错误信息
    FATAL = 4,      ///< 致命错误
    OFF   = 5,      ///< 关闭日志
};

/**
 * @brief 日志管理器类
 * 
 * 单例模式实现的日志管理器
 * 
 * 关键特性：
 * 1. 单例模式：全局唯一实例
 * 2. 线程安全：写入时加锁
 * 3. 日志级别过滤：低于设定级别的日志不输出
 */
class Logger {
public:
    /**
     * @brief 获取单例实例
     * 
     * 使用 Meyer's Singleton 模式，线程安全且延迟初始化
     */
    static Logger& instance() {
        static Logger logger;
        return logger;
    }
    
    /**
     * @brief 设置日志级别
     * 
     * @param level 日志级别
     */
    void set_level(LogLevel level) {
        level_.store(level, std::memory_order_relaxed);
    }
    
    /**
     * @brief 设置日志级别（从字符串）
     * 
     * @param level_str 日志级别字符串 (debug/info/warn/error/fatal/off)
     */
    void set_level(const std::string& level_str);
    
    /**
     * @brief 获取当前日志级别
     */
    LogLevel get_level() const {
        return level_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief 检查日志级别是否启用
     * 
     * @param level 要检查的级别
     * @return true 如果该级别的日志会被输出
     */
    bool is_enabled(LogLevel level) const {
        return static_cast<int>(level) >=
               static_cast<int>(level_.load(std::memory_order_relaxed));
    }
    
    /**
     * @brief 记录日志
     * 
     * @param level 日志级别
     * @param file 源文件名
     * @param line 行号
     * @param func 函数名
     * @param fmt 格式化字符串
     * @param ... 可变参数
     */
    void log(LogLevel level, const char* file, int line, 
             const char* func, const char* fmt, ...)
        __attribute__((format(printf, 6, 7)));
    
private:
    Logger() : level_(LogLevel::INFO) {}
    
    // 禁止拷贝和移动
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    /**
     * @brief 获取日志级别的字符串表示
     */
    static const char* get_level_str(LogLevel level);
    
    std::atomic<LogLevel> level_;  ///< 当前日志级别（多核读取）
    std::mutex mutex_;      ///< 输出互斥锁
};

/**
 * @brief 限速判断：距离上次输出不足 interval_sec 秒时返回 false
 *
 * @param last 调用点私有的上次输出时间（秒，0 表示从未输出）
 */
bool log_ratelimit_pass(std::atomic<int64_t>& last, int64_t interval_sec);

} // namespace l4lb

// ============================================================================
// 日志宏定义
// 
// 使用宏定义有以下优点：
// 1. 自动填充文件名、行号、函数名
// 2. 条件编译可完全移除调试日志
// 3. 短路求值避免不必要的参数计算
// ============================================================================

/// 先判断级别再求值参数：级别未开启时，ip_to_string() 等参数表达式不会执行
#define L4LB_LOG(level, fmt, ...) \
    do { \
        if (__builtin_expect(l4lb::Logger::instance().is_enabled(level), 0)) { \
            l4lb::Logger::instance().log(level, \
                __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

/// 调试日志（定义 NDEBUG 时完全移除）
#ifndef NDEBUG
#define LOG_DEBUG(fmt, ...) L4LB_LOG(l4lb::LogLevel::DEBUG, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

/// 信息日志
#define LOG_INFO(fmt, ...) L4LB_LOG(l4lb::LogLevel::INFO, fmt, ##__VA_ARGS__)

/// 警告日志
#define LOG_WARN(fmt, ...) L4LB_LOG(l4lb::LogLevel::WARN, fmt, ##__VA_ARGS__)

/// 错误日志
#define LOG_ERROR(fmt, ...) L4LB_LOG(l4lb::LogLevel::ERROR, fmt, ##__VA_ARGS__)

/// 致命错误日志
#define LOG_FATAL(fmt, ...) L4LB_LOG(l4lb::LogLevel::FATAL, fmt, ##__VA_ARGS__)

/// 条件日志：满足条件时记录
#define LOG_IF(level, cond, fmt, ...) \
    do { \
        if (cond) { \
            L4LB_LOG(level, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

/**
 * @brief 限速日志：同一调用点每 interval_sec 秒最多输出一次
 *
 * 用于数据面上可能每包触发的告警（无后端、查不到 MAC 等），
 * 避免故障时每包加锁 fprintf 拖垮转发。
 */
#define LOG_RATELIMIT(level, interval_sec, fmt, ...) \
    do { \
        static std::atomic<int64_t> l4lb_rl_last_{0}; \
        if (__builtin_expect(l4lb::Logger::instance().is_enabled(level), 0) && \
            l4lb::log_ratelimit_pass(l4lb_rl_last_, (interval_sec))) { \
            l4lb::Logger::instance().log(level, \
                __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

/// 每 N 次记录一次（用于高频日志）
#define LOG_EVERY_N(level, n, fmt, ...) \
    do { \
        static int __log_count = 0; \
        if (++__log_count >= (n)) { \
            __log_count = 0; \
            l4lb::Logger::instance().log(level, \
                __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#endif // L4LB_COMMON_LOGGER_H
