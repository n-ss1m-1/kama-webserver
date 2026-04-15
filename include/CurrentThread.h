#pragma once

#include <unistd.h>
#include <sys/syscall.h>

/**
 * CurrentThread 命名空间
 * 作用：提供当前线程信息的线程局部缓存，避免频繁系统调用
 * 设计思想：使用thread_local变量缓存线程特定数据，提升性能
 */
namespace CurrentThread
{
    extern thread_local int t_cachedTid; // 保存tid缓存 因为系统调用非常耗时 拿到tid后将其保存

    void cacheTid();

    /**
     * 获取当前线程ID（内联函数，高性能）
     * 
     * 使用__builtin_expect进行分支预测优化：
     * - 大多数情况下t_cachedTid != 0（已缓存），直接返回
     * - 只有第一次调用时t_cachedTid == 0，需要执行cacheTid()
     * 
     * __builtin_expect(expr, value) 含义：
     * - 告诉编译器expr表达式最可能的结果是value
     * - 这里表示t_cachedTid == 0的情况很少发生（value=0）
     */
    inline int tid() // 内联函数只在当前文件中起作用
    {
        if (__builtin_expect(t_cachedTid == 0, 0)) // __builtin_expect 是一种底层优化 此语句意思是如果还未获取tid 进入if 通过cacheTid()系统调用获取tid
        {
            cacheTid();
        }
        return t_cachedTid;
    }
}