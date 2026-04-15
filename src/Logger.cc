//Logger.cc
#include "Logger.h"
#include "CurrentThread.h"

// 线程局部存储命名空间
// 用于存储每个线程独立的数据，避免多线程竞争
namespace ThreadInfo
{
    thread_local char t_errnobuf[512]; // 每个线程独立的错误信息缓冲
    thread_local char t_timer[64];     // 每个线程独立的时间格式化缓冲区
    thread_local time_t t_lastSecond;  // 每个线程记录上次格式化的时间

}

const char *getErrnoMsg(int savedErrno)
{
    // 参数: errno值+缓冲区指针+缓冲区大小
    return strerror_r(savedErrno, ThreadInfo::t_errnobuf, sizeof(ThreadInfo::t_errnobuf));
}

// 根据Level 返回level_名字
const char *getLevelName[Logger::LogLevel::LEVEL_COUNT]{
    "TRACE ",
    "DEBUG ",
    "INFO  ",
    "WARN  ",
    "ERROR ",
    "FATAL ",
};

/**
 * 默认的日志输出函数
 * 将日志内容写入标准输出流(stdout)
 * @param data 要输出的日志数据
 * @param len 日志数据的长度W
 */
static void defaultOutput(const char *data, int len)
{
    fwrite(data, len, sizeof(char), stdout);        // 使用fwrite一次性写入，比多次写入更高效
}

/**
 * 默认的刷新函数
 * 刷新标准输出流的缓冲区,确保日志及时输出
 * 在发生错误或需要立即看到日志时会被调用
 */
static void defaultFlush()
{
    fflush(stdout);     // 强制刷新标准输出的缓冲区
}


Logger::OutputFunc Logger::g_output = defaultOutput;
Logger::FlushFunc Logger::g_flush = defaultFlush;


Logger::Impl::Impl(Logger::LogLevel level, int savedErrno, const char *filename, int line)
    : time_(Timestamp::now()),      // 记录日志创建的时间戳
      stream_(),                    // 初始化LogStream对象
      level_(level),                // 设置日志级别
      line_(line),                  // 设置源代码行号
      basename_(filename)           // 创建SourceFile对象，提取纯文件名
{
    // 1. 格式化时间戳，作为日志消息的开头
    // 时间格式：YYYY/MM/DD HH:MM:SS.uuuuuu
    formatTime();
    
    // 2. 写入日志级别标签
    // 使用GeneralTemplate避免重复计算字符串长度
    // getLevelName[level]返回6个字符的字符串，包括末尾空格
    stream_ << GeneralTemplate(getLevelName[level], 6);
    
    // 3. 如果有errno，添加错误信息
    // savedErrno != 0 表示有系统错误
    if (savedErrno != 0)
    {
        // 格式：错误描述 (errno=错误码)
        // 例如："No such file or directory (errno=2) "
        stream_ << getErrnoMsg(savedErrno) 
                << " (errno=" << savedErrno << ") ";
    }
}

/**
 * 格式化时间戳
 * 将当前时间格式化为字符串：YYYY/MM/DD HH:MM:SS.uuuuuu
 * 并将其写入日志流
 * 
 * 注意：
 * 1. 使用线程局部缓冲区避免多线程竞争
 * 2. 缓存上次格式化的秒数，优化性能
 */
void Logger::Impl::formatTime()
{
    // 获取当前时间戳
    Timestamp now = Timestamp::now();
    
    // 分离秒和微秒部分
    // 秒数：自1970-01-01以来的秒数
    time_t seconds = static_cast<time_t>(
        now.microSecondsSinceEpoch() / Timestamp::kMicroSecondsPerSecond);
    
    // 微秒数：剩余不足1秒的部分
    int microseconds = static_cast<int>(
        now.microSecondsSinceEpoch() % Timestamp::kMicroSecondsPerSecond);
    
    // 将秒数转换为本地时间结构体
    // localtime函数是线程安全的（在大多数实现中）
    struct tm *tm_timer = localtime(&seconds);
    
    // 格式化日期和时间部分到线程局部缓冲区
    // 格式：YYYY/MM/DD HH:MM:SS
    snprintf(ThreadInfo::t_timer, sizeof(ThreadInfo::t_timer), 
             "%4d/%02d/%02d %02d:%02d:%02d",
             tm_timer->tm_year + 1900,  // tm_year是从1900年开始的年数
             tm_timer->tm_mon + 1,      // tm_mon范围是0-11，需要+1
             tm_timer->tm_mday,         // 月中的日期（1-31）
             tm_timer->tm_hour,         // 小时（0-23）
             tm_timer->tm_min,          // 分钟（0-59）
             tm_timer->tm_sec);         // 秒（0-60，60表示闰秒）
    
    // 更新线程局部存储的上次格式化时间
    // 这个值在其他地方可能会被用于优化（如避免重复格式化）
    ThreadInfo::t_lastSecond = seconds;
    
    // 格式化微秒部分
    // 格式：6位微秒 + 1个空格(微秒包含了秒)
    char buf[32] = {0};
    snprintf(buf, sizeof(buf), "%06d ", microseconds);
    
    // 将格式化好的时间写入日志流
    // 注意：只取t_timer的前17个字符，即"YYYY/MM/DD HH:MM:"
    stream_ << GeneralTemplate(ThreadInfo::t_timer, 17)  // 日期和时间部分
            << GeneralTemplate(buf, 7);                  // 微秒部分
}

/**
 * 完成一条日志消息
 * 在日志消息末尾添加文件名和行号信息
 * 格式：- 文件名:行号\n
 * 
 * 注意：这个方法在Logger析构函数中被调用
 */
void Logger::Impl::finish()
{
    // 添加分隔符" - "
    stream_ << " - " 
            // 添加文件名（通过GeneralTemplate避免strlen）
            << GeneralTemplate(basename_.data_, basename_.size_)
            // 添加冒号和行号
            << ':' << line_
            // 添加换行符
            << '\n';
}

/**
 * Logger类的构造函数
 * 主要工作是创建Impl对象
 * 
 * @param filename 源代码文件名
 * @param line 源代码行号
 * @param level 日志级别
 * 
 * 注意：这里savedErrno参数固定为0，表示默认不包含errno信息
 * 如果需要包含errno信息，需要在创建Logger时传入
 */
Logger::Logger(const char *filename, int line, LogLevel level) 
    : impl_(level, 0, filename, line)  // 初始化impl_对象
{
    // 构造函数体为空，所有工作都在成员初始化列表中完成
    // 这是RAII（Resource Acquisition Is Initialization）模式的典型应用
}

/**
 * Logger类的析构函数
 * ▲在对象销毁时输出完整的日志消息
 * 
 * 注意：这是日志输出的关键时机
 * 通过RAII模式，确保日志消息总是被输出
 */
Logger::~Logger()
{
    // 1. 添加文件名和行号后缀
    impl_.finish();
    
    // 2. 获取格式化好的日志内容
    // 注意：这里返回的是Buffer的常量引用，避免拷贝
    const LogStream::Buffer &buffer = stream().buffer();
    
    // 3. 调用全局输出函数输出日志
    // 默认情况下，g_output指向defaultOutput（输出到stdout）
    // 可以通过Logger::setOutput()替换为自定义输出函数---▲(传递一)调用AsyncLogging的append,输出定向到后端(main.cc处可见)
    g_output(buffer.data(), buffer.length());
    
    // 4. 如果是FATAL级别的日志，执行特殊处理
    if (impl_.level_ == FATAL)
    {
        // 先刷新输出缓冲区，确保所有日志都已输出
        g_flush();
        
        // 终止程序执行
        // abort()会生成core dump（如果系统配置允许）
        abort();
    }
    
    // 注意：非FATAL级别的日志不会自动刷新
    // 刷新操作由输出函数或日志系统自己管理
}

