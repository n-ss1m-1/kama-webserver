//Logger.h
#pragma once

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <errno.h>
#include "LogStream.h"
#include <functional>
#include "Timestamp.h"

//#define OPEN_LOGGING 


// SourceFile的作用是提取文件名
class SourceFile
{
public:
    explicit SourceFile(const char* filename)
        : data_(filename)
    {
        /**
         * strrchr找出filename中出现/最后一次的位置，从而获取具体的文件名
         * 2022/10/26/test.log
         */
        const char* slash = strrchr(filename, '/');
        if (slash)
        {
            data_ = slash + 1;
        }
        size_ = static_cast<int>(strlen(data_));
    }

    const char* data_;      //具体的文件名
    int size_;
};


// 主日志类，提供用户接口
class Logger
{
public:
   // 日志级别枚举
   enum LogLevel
    {
        TRACE,          // 跟踪信息，最详细
        DEBUG,          // 调试信息
        INFO,           // 一般信息
        WARN,           // 警告信息
        ERROR,          // 错误信息
        FATAL,          // 致命错误，程序通常会终止
        LEVEL_COUNT,    // 级别总数，用于数组大小
    };
    
    // 构造函数：记录日志位置和级别
    Logger(const char *filename, int line, LogLevel level);
    
    // 析构函数：输出完整的日志消息
    ~Logger();
    
    // 获取日志流，用于通过<<操作符输出日志内容
    LogStream& stream() { return impl_.stream_; }

    /*
    两种方法：
    1.在.cc中定义全局输出函数 使用static方法进行set
    2.在类内定义静态输出函数  使用static方法进行set
    由于每次输出日志使用日志宏->Logger临时对象->需要使用【全局】输出函数/【静态】输出函数->需要使用static方法进行设置
    */

    // 输出函数和刷新缓冲区函数的类型定义
    using OutputFunc = std::function<void(const char* msg, int len)>;
    using FlushFunc = std::function<void()>;

    static OutputFunc g_output;
    static FlushFunc g_flush;
    
    // 静态方法：设置全局/静态输出函数      
    static void setOutput(OutputFunc) {g_output = outputFunc;}
    
    // 静态方法：设置全局/静态刷新函数
    static void setFlush(FlushFunc) {g_flush = FlushFunc;}

private:
    // 内部实现类，封装日志的具体实现细节
    class Impl
    {
    public:
        using LogLevel=Logger::LogLevel;  // 使用外层类的LogLevel
        
        // 构造函数：初始化日志消息的各个部分
        Impl(LogLevel level,int savedErrno,const char *filename, int line);
        
        // 格式化时间戳
        void formatTime();
        
        // 添加一条log消息的后缀（文件名:行号）
        void finish();

        Timestamp time_;      // 时间戳
        LogStream stream_;    // 日志流，用于存储日志内容
        LogLevel level_;      // 日志级别
        int line_;            // 行号
        SourceFile basename_; // 文件名（不含路径）
    };

private:
    Impl impl_;  // 内部实现对象
};

// 获取errno信息
const char* getErrnoMsg(int savedErrno);


/**
 * 日志宏定义
 * 当日志等级小于对应等级才会输出
 * 例如：设置等级为FATAL，则DEBUG和INFO等级的日志就不会输出
 * 
 * 原理：
 * LOG_INFO 宏展开为：Logger(__FILE__, __LINE__, Logger::INFO).stream()
 * 1. 创建一个临时Logger对象
 * 2. 调用stream()获取LogStream引用
 * 3. 通过<<操作符写入日志内容
 * 4. Logger对象析构，输出完整日志
 */
#ifdef OPEN_LOGGING
    // 各种级别的日志宏
    #define LOG_DEBUG Logger(__FILE__, __LINE__, Logger::DEBUG).stream()        //预定义的宏：__FILE__代表当前运行的文件名  __LINE__代表当前运行的行号
    #define LOG_INFO Logger(__FILE__, __LINE__, Logger::INFO).stream()
    #define LOG_WARN Logger(__FILE__, __LINE__, Logger::WARN).stream()
    #define LOG_ERROR Logger(__FILE__, __LINE__, Logger::ERROR).stream()
    #define LOG_FATAL Logger(__FILE__, __LINE__, Logger::FATAL).stream()
#else
    // 关闭日志时，返回一个空的LogStream，避免日志输出
    //#define LOG(level) LogStream()
    #define LOG_DEBUG LogStream()
    #define LOG_INFO LogStream()
    #define LOG_WARN LogStream()
    #define LOG_ERROR LogStream()
    #define LOG_FATAL LogStream()
#endif
