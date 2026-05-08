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

#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <mutex>
#include <string>
#include <memory>

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
        level_ = level;
    }
    
    /**
     * @brief 设置日志级别（从字符串）
     * 
     * @param level_str 日志级别字符串 (debug/info/warn/error)
     */
    void set_level(const std::string& level_str) {
        if (level_str == "debug") level_ = LogLevel::DEBUG;
        else if (level_str == "info") level_ = LogLevel::INFO;
        else if (level_str == "warn") level_ = LogLevel::WARN;
        else if (level_str == "error") level_ = LogLevel::ERROR;
        else if (level_str == "fatal") level_ = LogLevel::FATAL;
        else if (level_str == "off") level_ = LogLevel::OFF;
    }
    
    /**
     * @brief 获取当前日志级别
     */
    LogLevel get_level() const {
        return level_;
    }
    
    /**
     * @brief 检查日志级别是否启用
     * 
     * @param level 要检查的级别
     * @return true 如果该级别的日志会被输出
     */
    bool is_enabled(LogLevel level) const {
        return static_cast<int>(level) >= static_cast<int>(level_);
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
             const char* func, const char* fmt, ...) {
        // 级别过滤
        if (!is_enabled(level)) {
            return;
        }
        
        // 获取当前时间
        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);
        
        // 获取级别字符串
        const char* level_str = get_level_str(level);
        
        // 从文件路径中提取文件名
        const char* filename = file;
        const char* p = file;
        while (*p) {
            if (*p == '/' || *p == '\\') {
                filename = p + 1;
            }
            ++p;
        }
        
        // 格式化用户消息
        char msg_buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
        va_end(args);
        
        // 加锁输出
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fprintf(stderr, "[%s] [%s] [%s:%d %s] %s\n",
                    time_str, level_str, filename, line, func, msg_buf);
            fflush(stderr);
        }
    }
    
private:
    Logger() : level_(LogLevel::INFO) {}
    
    // 禁止拷贝和移动
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    /**
     * @brief 获取日志级别的字符串表示
     */
    static const char* get_level_str(LogLevel level) {
        switch (level) {
            case LogLevel::DEBUG: return "DEBUG";
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
            case LogLevel::FATAL: return "FATAL";
            default:              return "?????";
        }
    }
    
    LogLevel level_;        ///< 当前日志级别
    std::mutex mutex_;      ///< 输出互斥锁
};

} // namespace l4lb

// ============================================================================
// 日志宏定义
// 
// 使用宏定义有以下优点：
// 1. 自动填充文件名、行号、函数名
// 2. 条件编译可完全移除调试日志
// 3. 短路求值避免不必要的参数计算
// ============================================================================

/// 调试日志（Release 编译可移除）
#ifndef NDEBUG
#define LOG_DEBUG(fmt, ...) \
    l4lb::Logger::instance().log(l4lb::LogLevel::DEBUG, \
        __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

/// 信息日志
#define LOG_INFO(fmt, ...) \
    l4lb::Logger::instance().log(l4lb::LogLevel::INFO, \
        __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/// 警告日志
#define LOG_WARN(fmt, ...) \
    l4lb::Logger::instance().log(l4lb::LogLevel::WARN, \
        __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/// 错误日志
#define LOG_ERROR(fmt, ...) \
    l4lb::Logger::instance().log(l4lb::LogLevel::ERROR, \
        __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/// 致命错误日志
#define LOG_FATAL(fmt, ...) \
    l4lb::Logger::instance().log(l4lb::LogLevel::FATAL, \
        __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

/// 条件日志：满足条件时记录
#define LOG_IF(level, cond, fmt, ...) \
    do { \
        if (cond) { \
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
