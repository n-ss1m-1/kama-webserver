//AsyncLogging.h
#pragma once

#include "noncopyable.h"
#include "Thread.h"
#include "FixedBuffer.h"
#include "LogStream.h"
#include "LogFile.h"

#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>

/**
 * AsyncLogging 类 - 异步日志系统的核心类
 * 
 * 职责：
 * 1. 提供前端接口，接收日志消息
 * 2. 管理内存缓冲区，缓冲日志消息
 * 3. 启动后端线程，将缓冲区中的日志写入文件
 * 4. 协调前端（生产者）和后端（消费者）的并发访问
 * 
 * 设计模式：生产者-消费者模式
 * 前端线程：生产者，生成日志消息
 * 后端线程：消费者，将日志写入文件
 */
class AsyncLogging
{
public:
    /**
     * 构造函数
     * @param basename 日志文件基本名称
     * @param rollSize 日志文件滚动大小（字节）
     * @param flushInterval 刷新间隔（秒），默认3秒
     * 
     * 初始化异步日志系统，设置日志文件的基本参数
     */
    AsyncLogging(const std::string &basename, off_t rollSize, int flushInterval=3);
    
    /**
     * 析构函数
     * 如果异步日志还在运行，则停止它
     * 注意：调用stop()会通知后端线程退出
     */
    ~AsyncLogging()
    {
        if (running_)
        {
            stop();
        }
    }
    
    /**
     * 前端接口：将日志消息追加到缓冲区
     * @param logline 日志消息的指针
     * @param len 日志消息的长度
     * 
     * 被多个前端线程调用，需要线程安全
     * 如果当前缓冲区空间不足，会触发缓冲区交换
     */
    void append(const char *logline, int len);
    
    /**
     * 启动异步日志系统
     * 设置运行标志，启动后端线程
     */
    void start()
    {
        running_ = true;     // 设置运行标志
        thread_.start();     // 启动后端线程
    }
    
    /**
     * 停止异步日志系统
     * 设置运行标志为false，通知后端线程
     * 注意：不会等待后端线程完全退出，析构函数会等待
     */
    void stop()
    {
        running_ = false;    // 清除运行标志
        cond_.notify_one();  // 通知后端线程，避免其阻塞在条件变量上
    }

private:
    // 大缓冲区类型，用于存储多个日志消息
    // kLargeBufferSize = 4000 * 1000 = 4MB
    using LargeBuffer = FixedBuffer<kLargeBufferSize>;
    
    // 缓冲区向量，存储unique_ptr智能指针
    // 管理多个已满的缓冲区
    using BufferVector = std::vector<std::unique_ptr<LargeBuffer>>;
    
    // 缓冲区指针类型
    // BufferVector::value_type 是 std::vector<std::unique_ptr<Buffer>> 的元素类型
    // 也就是 std::unique_ptr<Buffer>
    using BufferPtr = BufferVector::value_type;
    
    /**
     * 后端线程函数
     * 负责将缓冲区中的日志写入文件
     * 循环执行，直到running_为false
     */
    void threadFunc();
    
    // 成员变量
    
    std::atomic<bool> running_; // 运行标志，原子操作保证线程安全
                               // true: 正在运行
                               // false: 停止运行
    
    const std::string basename_; // 日志文件基本名称
    const off_t rollSize_;      // 日志文件滚动大小（字节）
    const int flushInterval_;  // 日志刷新时间间隔（秒）
                               // 后端线程等待的最大时间，超过这个时间即使缓冲区未满也会刷新
    
    Thread thread_;             // 后端线程对象，执行threadFunc函数
    
    std::mutex mutex_;          // 互斥锁，保护共享数据（缓冲区）
    std::condition_variable cond_; // 条件变量，用于前后端线程同步
    
    /**
     * 缓冲区管理（四缓冲区技术）：
     * 
     * 1. currentBuffer_: 当前正在使用的缓冲区
     *    - 前端线程正在向此缓冲区写入日志
     *    - 当缓冲区满时，会将其移入buffers_队列
     *    
     * 2. nextBuffer_: 备用缓冲区
     *    - 当currentBuffer_满时，立即用nextBuffer_替换
     *    - 避免在前端创建新缓冲区的开销
     *    
     * 3. buffers_: 已满的缓冲区队列
     *    - 存储等待写入文件的已满缓冲区
     *    - 后端线程从此队列取出缓冲区写入文件
     *    
     * 设计原理：减少内存分配和锁竞争
     * - 预先分配多个缓冲区，避免运行时动态分配
     * - 缓冲区交换快速，减少锁持有时间
     */
    BufferPtr currentBuffer_;  // 当前缓冲区（前端正在写入）
    BufferPtr nextBuffer_;     // 备用缓冲区
    BufferVector buffers_;     // 已满缓冲区队列（待写入文件）
};