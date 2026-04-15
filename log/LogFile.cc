//LogFile.cc
#include "LogFile.h"
LogFile::LogFile(const std::string &basename,
                 off_t rollsize,
                 int flushInterval,
                 int checkEveryN ) : basename_(basename),
                                           rollsize_(rollsize),
                                           flushInterval_(flushInterval),
                                           checkEveryN_(checkEveryN),
                                           startOfPeriod_(0),           //time_t -> 整数类型 秒数
                                           lastRoll_(0),
                                           lastFlush_(0)
{
    // 重新启动时，可能没有log文件，因此在构建logFile对象，直接调用rollfile()创建一个新的log文件
    rollFile();
}

LogFile::~LogFile() = default;



void LogFile::append(const char *data, int len)
{
    std::lock_guard<std::mutex> lg(mutex_);
    appendInlock(data, len);
}

void LogFile::appendInlock(const char *data, int len)
{
    file_->append(data, len);
    
    time_t now = time(NULL); // 当前时间
    ++count_;

    // 1. 判断是否需要滚动日志
    if (file_->writtenBytes() > rollsize_)      // 1.1通过写入的字节数判断
    {
        rollFile();
    }
    else if (count_ >= checkEveryN_)            // 1.2达到写入次数阈值后，进行检查
    {
        count_ = 0;

        // 1.2基于时间周期滚动日志
        time_t thisPeriod = now / kRollPerSeconds_ * kRollPerSeconds_;  //天数 向下取整
        if (thisPeriod != startOfPeriod_)
        {
            rollFile();
        }
    }

    // 2. 判断是否需要刷新日志（独立的刷新逻辑）
    if (now - lastFlush_ > flushInterval_)
    {
        lastFlush_ = now;
        flush();
    }
}

void LogFile::flush()
{
    file_->flush();
}

// 滚动日志
bool LogFile::rollFile()
{
    time_t now = 0;
    std::string filename = getLogFileName(basename_, &now);
    time_t start = now / kRollPerSeconds_ * kRollPerSeconds_;       //天数 向下取整
    if (now > lastRoll_)
    {
        lastFlush_ = now;
        lastRoll_ = now;
        startOfPeriod_ = start;
        // (unique_ptr的方法)让file_重新指向一个名为filename的文件，相当于新建了一个文件(new)，原文件的FileUtil自动析构->fclose()==fflush()+close() 会自动刷新缓冲区并关闭文件
        file_.reset(new FileUtil(filename));
        return true;
    }
    return false;
}

// 日志格式basename+now+".log"
std::string LogFile::getLogFileName(const std::string &basename, time_t *now)
{
    std::string filename;
    filename.reserve(basename.size() + 64);
    filename = basename;

    char timebuf[32];
    struct tm tm;                   // C标准库中表示日历时间的结构体
    *now = time(NULL);              // 获取当前时间  1970-01-01 00:00:00 UTC到现在的【秒数】（Unix时间戳）
    localtime_r(now, &tm);          // 输入时间戳 输出分解时间
    strftime(timebuf, sizeof(timebuf), ".%Y%m%d-%H%M%S", &tm);  //将分解时间格式化为特定格式的时间字符串

    filename += timebuf;
    filename += ".log";
    return filename;
}



