// 包含AsyncLogging类的头文件
#include "AsyncLogging.h"
#include <stdio.h>  // 用于标准输出

/**
 * AsyncLogging 构造函数
 * 初始化异步日志系统的所有组件
 * 
 * @param basename 日志文件基本名称
 * @param rollSize 日志文件滚动大小（字节）
 * @param flushInterval 刷新间隔（秒），默认3秒
 * 
 * 初始化成员变量，创建线程对象，准备缓冲区
 */
AsyncLogging::AsyncLogging(const std::string &basename, off_t rollSize, int flushInterval)
    : running_(false),                    // 运行标志初始化为false
      basename_(basename),                // 日志文件基本名称
      rollSize_(rollSize),                // 日志文件滚动大小
      flushInterval_(flushInterval),      // 刷新间隔，默认3秒
      thread_(std::bind(&AsyncLogging::threadFunc, this), "Logging"),  // 创建线程，绑定到threadFunc
      mutex_(),                           // 默认构造互斥锁
      cond_(),                            // 默认构造条件变量
      currentBuffer_(new LargeBuffer),    // 创建当前缓冲区（4MB）
      nextBuffer_(new LargeBuffer),       // 创建备用缓冲区（4MB）
      buffers_()                          // 默认构造缓冲区队列
{
    // 初始化缓冲区，清零内容
    currentBuffer_->bzero();
    nextBuffer_->bzero();
    
    // 预分配缓冲区队列的容量为16
    // 这限制了前端最多可以积累16个已满的缓冲区
    // 防止内存无限制增长
    buffers_.reserve(16);
}

/**
 * 前端接口：追加日志消息
 * 被多个前端线程调用，将日志消息写入缓冲区
 * 
 * @param logline 日志消息指针
 * @param len 日志消息长度
 * 
 * 工作流程：
 * 1. 加锁保护共享数据
 * 2. 如果当前缓冲区有足够空间，直接追加
 * 3. 如果当前缓冲区空间不足，将其移入队列，使用备用缓冲区
 * 4. 通知后端线程
 */
void AsyncLogging::append(const char *logline, int len)
{
    // 加锁，保护共享数据：currentBuffer_, nextBuffer_, buffers_
    std::lock_guard<std::mutex> lg(mutex_);
    
    // 如果当前缓冲区有足够空间，直接追加
    // 这是快速路径，大多数情况下会走这里
    if (currentBuffer_->avail() > static_cast<size_t>(len))
    {
        currentBuffer_->append(logline, len);
    }
    else
    {
        // 慢速路径：当前缓冲区空间不足
        
        // 1. 将当前缓冲区移入已满缓冲区队列
        // std::move转移所有权，currentBuffer_变为空指针
        buffers_.push_back(std::move(currentBuffer_));
        
        // 2. 检查是否有备用缓冲区
        if (nextBuffer_)
        {
            // 有备用缓冲区，将其作为新的当前缓冲区
            currentBuffer_ = std::move(nextBuffer_);
        }
        else
        {
            // 没有备用缓冲区（极少发生），创建新的缓冲区
            // 这种情况发生在后端线程未能及时归还缓冲区时
            currentBuffer_.reset(new LargeBuffer);      //unique_ptr的方法 重新开辟一块缓冲区
        }
        
        // 3. 将当前日志消息追加到新的当前缓冲区
        currentBuffer_->append(logline, len);
        
        // 4. 通知后端线程有新的数据可写
        // 只通知一个等待的线程（只有一个后端线程）
        cond_.notify_one();
    }
}

/**
 * 后端线程函数
 * 负责将缓冲区中的日志数据写入文件
 * 循环执行，直到running_变为false
 * 
 * 工作流程：
 * 1. 创建日志文件对象
 * 2. 准备两个新缓冲区用于替换
 * 3. 循环等待和处理缓冲区
 * 4. 写入文件并清理
 */
void AsyncLogging::threadFunc()
{
    // ▲创建LogFile对象，用于写入磁盘(传递二)
    // 参数：基础文件名，滚动大小
    LogFile output(basename_, rollSize_);
    
    // 创建两个新的缓冲区，用于替换前端的缓冲区
    // 这样后端始终有两个备用缓冲区可以给前端使用
    BufferPtr newbuffer1(new LargeBuffer);  // 用于替换currentBuffer_
    BufferPtr newbuffer2(new LargeBuffer);  // 用于替换nextBuffer_
    
    // 清零缓冲区
    newbuffer1->bzero();
    newbuffer2->bzero();
    
    // 准备缓冲区向量，用于从前端交换已满的缓冲区
    // 容量16，与前端保持一致
    BufferVector buffersToWrite;
    buffersToWrite.reserve(16);
    
    // 主循环：只要running_为true就继续运行
    while (running_)
    {
        // 第一阶段：收集待写入的缓冲区
        {
            // 加锁，保护共享数据
            // 使用unique_lock，因为需要与条件变量配合
            std::unique_lock<std::mutex> lg(mutex_);
            
            // 如果缓冲区队列为空，等待条件变量
            // 等待最多flushInterval_秒（默认3秒）
            // 即使没有新日志，也会定期刷新
            if (buffers_.empty())
            {
                // 等待条件变量
                // 两种情况会返回：
                // 1. 收到通知（前端调用了cond_.notify_one()）
                // 2. 超时（flushInterval_秒）
                cond_.wait_for(lg, std::chrono::seconds(flushInterval_));
            }
            
            // 无论是因为通知还是超时，将当前缓冲区也加入队列
            // 即使currentBuffer_可能未满，也要将其加入待写队列
            // 这样可以保证定期刷新，避免日志长时间停留在内存中
            buffers_.push_back(std::move(currentBuffer_));          //先是currentBuffer_放入buffers_ -> buffers与buffersToWrite交换
            
            // 用newbuffer1替换currentBuffer_，前端继续使用新的缓冲区
            currentBuffer_ = std::move(newbuffer1);
            
            // 如果前端没有备用缓冲区，也补充一个
            if (!nextBuffer_)
            {
                nextBuffer_ = std::move(newbuffer2);
            }
            
            // 交换buffers_和buffersToWrite
            // 这样：
            // 1. buffers_变为空，前端可以继续添加新的已满缓冲区
            // 2. buffersToWrite包含所有待写入的缓冲区
            // 交换是O(1)操作，效率高
            buffersToWrite.swap(buffers_);
            
            // ▲注意：离开作用域，锁自动释放
            // 前端线程可以立即继续写入，不阻塞
        }
        
        // 第二阶段：写入磁盘（无需加锁，因为buffersToWrite是线程局部数据）
        // 遍历所有待写入缓冲区，将其内容写入文件
        for (auto &buffer : buffersToWrite)
        {
            // 将缓冲区中的数据追加到日志文件
            output.append(buffer->data(), buffer->length());
        }
        
        // 第三阶段：回收缓冲区
        // 如果待写缓冲区数量超过2，只保留两个
        // 这是为了避免内存占用过多
        if (buffersToWrite.size() > 2)
        {
            // 只保留前两个缓冲区，其余的自动销毁
            // 因为存储的是unique_ptr，会自动释放内存
            buffersToWrite.resize(2);
        }
        
        // 回收缓冲区，供下次使用
        // 如果newbuffer1为空（已经被前端使用），从buffersToWrite中取一个
        if (!newbuffer1)
        {
            // 从buffersToWrite末尾取一个缓冲区
            newbuffer1 = std::move(buffersToWrite.back());
            buffersToWrite.pop_back();
            // 重置缓冲区，清空内容
            newbuffer1->reset();
        }
        
        // 同样的逻辑处理newbuffer2
        if (!newbuffer2)
        {
            newbuffer2 = std::move(buffersToWrite.back());
            buffersToWrite.pop_back();
            newbuffer2->reset();
        }
        
        // 清空buffersToWrite
        // 注意：此时buffersToWrite中可能还有缓冲区
        // 但它们是多余的，会被自动销毁
        buffersToWrite.clear();
        
        // 刷新文件缓冲区，确保数据写入磁盘
        output.flush();
    }
    
    // 循环结束后（running_变为false），最后刷新一次
    // 确保所有数据都写入磁盘
    output.flush();
}
/*
轮换顺序：
    buffers_     ←1      currentBuffer_      nextBuffer_
       4↓                    2↑                 3↑
    buffersToWrite   5→  newBuffer1          newBuffer2

*/

/*
区分lock_guard和unique_lock:
unique_lock可以手动加锁、手动解锁、兼容条件变量 
lock_guard则不具备上述功能，加锁后只有离开作用域 析构后 才能解锁
*/

/*
用户空间
    ↓
LogStream::FixedBuffer (4KB, 栈上, 单条日志)
    ↓ 拷贝 (Logger析构时)                   (调用AsyncLogging的append(main.cc处))
AsyncLogging::LargeBuffer (4MB, 堆上, 批量收集)
    ↓ 交换所有权 (后端线程)                 (push_back  move  swap)
buffersToWrite 局部向量
    ↓ 批量拷贝                              output.append()
LogFile 的 FILE* 缓冲区 (64KB, 栈上)
    ↓ fwrite_unlocked() 系统调用
内核空间
    ↓                                       output.flush()
页缓存
    ↓
磁盘调度
    ↓
物理磁盘
*/