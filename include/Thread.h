#pragma once

#include <functional>
#include <thread>
#include <memory>
#include <unistd.h>
#include <string>
#include <atomic>

#include "noncopyable.h"

class Thread : noncopyable
{
public:
    //线程的回调函数
    using ThreadFunc = std::function<void()>;

    explicit Thread(ThreadFunc, const std::string &name = std::string());
    ~Thread();

    void start();
    void join();

    //状态查询接口
    bool started() { return started_; }                 //是否已启动
    pid_t tid() const { return tid_; }                  //获取线程ID
    const std::string &name() const { return name_; }   //获取线程名称
    //获取全局线程数
    static int numCreated() { return numCreated_; }     

private:
    //设置默认线程名称
    void setDefaultName();

    bool started_;          //线程是否已启动(防止重复启动)
    bool joined_;           //是否已调用join()等待线程结束
    std::shared_ptr<std::thread> thread_;   //底层线程对象(组合而非继承)
    pid_t tid_;             // 线程ID 在线程创建时再绑定
    ThreadFunc func_;       // 子线程的回调函数->EventLoopThread::threadFunc() 创建相应的eventloop
    std::string name_;      //线程名称
    static std::atomic_int numCreated_; //全局线程数目 静态成员(全局) 原子计数->线程安全
};