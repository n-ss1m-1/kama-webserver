#include <CurrentThread.h>

namespace CurrentThread
{
    /**
     * 定义线程局部变量t_cachedTid 缓存每个线程的ID
     * thread_local关键字确保每个线程有独立的变量副本
     * 初始值为0，表示尚未缓存线程ID
     */
    thread_local int t_cachedTid = 0; 

    /**
     * 缓存线程ID的实现
     * 如果尚未缓存（t_cachedTid == 0），则通过系统调用获取并缓存
     * 
     * 为什么使用syscall(SYS_gettid)而不是pthread_self()？
     * 1. gettid()返回的是内核级线程ID，在系统中唯一
     * 2. pthread_self()返回的是进程内线程标识，可能重复使用
     * 3. gettid()更适合日志记录和系统级标识
     */
    void cacheTid()
    {
        if (t_cachedTid == 0)
        {
            t_cachedTid = static_cast<pid_t>(::syscall(SYS_gettid)); // Ensure syscall and SYS_gettid are defined
        }
    }
}