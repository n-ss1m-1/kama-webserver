#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <stddef.h>

/*
动态增长的环形缓冲区(基础组件 只关心功能怎么实现)：
分区：  | prependable-预留以及已读空间  |  readable-待读数据  |  writable-可写空间 |        注：待读数据->读取后写到对端
[ prependable ] [     readable     ] [     writable     ]
↑              ↑                    ↑                   ↑
0         readerIndex_         writerIndex_        buffer_.size()
处理粘包拆包？
*/
class Buffer
{
public:
    static const size_t CheapPrepend = 8;       //缓冲区最前端 初始预留的空间大小 可用于添加协议头
    static const size_t InitialSize = 1024;    //默认缓冲区大小 1024->32768

    explicit Buffer(size_t initialSize = InitialSize)       //xxxxxbuffer初始容量 readerIndex_初始位置
        : buffer_(CheapPrepend + initialSize)
        , readerIndex_(CheapPrepend)
        , writerIndex_(CheapPrepend)
    {
    }

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }    //获取待读数据长度
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }  //获取可写空间长度
    size_t prependableBytes() const { return readerIndex_; }                //获取前置空间长度(预留+已读)

    // 返回待读数据的起始地址 即beginRead()
    const char *peek() const { return begin() + readerIndex_; }

    //获取可写空间的起始地址
    char *beginWrite() { return begin() + writerIndex_; }   
    const char *beginWrite() const { return begin() + writerIndex_; }

    //回收已读数据空间：移动读指针
    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            readerIndex_ += len; // 部分读取(已读len) 还剩下readerIndex+=len到writerIndex_的数据未读(保留)
        }
        else // len == readableBytes()
        {
            retrieveAll();      //全部读取：重置读写指针->初始位置
        }
    }
    
    //将所有指针重置到初始位置
    void retrieveAll()
    {
        readerIndex_ = CheapPrepend;
        writerIndex_ = CheapPrepend;
    }

    // 把onMessage函数收到的Buffer数据 转成string类型的数据返回
    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }
    std::string retrieveAsString(size_t len)
    {
        std::string result(peek(), len);
        retrieve(len); // 上面一句把缓冲区中可读的数据已经读取出来 这里肯定要对缓冲区进行复位操作
        return result;
    }

    // 确保有足够的可写空间 不足时扩容
    void ensureWritableBytes(size_t len)
    {
        if (writableBytes() < len)
        {
            makeSpace(len); // 扩容/腾出空间
        }
    }

    // 把[data, data+len]内存上的数据添加到writable缓冲区当中
    void append(const char *data, size_t len)
    {
        ensureWritableBytes(len);
        std::copy(data, data+len, beginWrite());
        writerIndex_ += len;
    }
    

    // 从fd上读取数据到buffer
    ssize_t readFd(int fd, int *saveErrno);
    // 将buffer上的数据 写到fd
    ssize_t writeFd(int fd, int *saveErrno);

private:
    // vector数组首元素的地址 也就是数组的起始地址
    char *begin() { return &*buffer_.begin(); }     //&*的原因：buffer_.begin()返回的是迭代器，而我们需要的是指针/地址。【通过&*将迭代器转换为指针】 
    const char *begin() const { return &*buffer_.begin(); }

    //扩容/腾出空间
    void makeSpace(size_t len)
    {
        /**
         * | CheapPrepend | xxx | reader | writer |                     // xxx标示reader中已读的部分
         * | CheapPrepend | reader ｜          len          |
         **/
        if (writableBytes() + prependableBytes()  < len + CheapPrepend) // 也就是说 len > xxx + writer的部分  //!!! ▲ +CheapPrepend 避免无符号数下溢
        {
            buffer_.resize(writerIndex_ + len);
        }
        else // 这里说明 len <= xxx + writer 把reader搬到从xxx开始 使得xxx后面是一段连续空间
        {
            size_t readable = readableBytes(); // readable = reader的长度
            // 将当前缓冲区中[readerIndex_ , writerIndex_)的数据
            // 拷贝到缓冲区起始位置CheapPrepend处，以便腾出更多的可写空间
            std::copy(begin() + readerIndex_,
                      begin() + writerIndex_,
                      begin() + CheapPrepend);
            readerIndex_ = CheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;            //读指针：指向下一个可读数据位置
    size_t writerIndex_;            //写指针：指向下一个可写位置
};


/*
将迭代器转换为指针/地址的方法： &*buffer_.begin()


函数是否应该使用const修饰：
1.只读取成员：应该
2.修改成员：不应该
3.返回内部指针：看情况(是否修改成员)
4.返回非const指针：不应该(允许外部修改成员)


什么时候返回 char* 或 const char* : 根据外部是否需要通过该指针修改数据来判断


size_t和ssize_t的区别：
size_t表示无符号整数|||ssize_t表示有符号整数
*/