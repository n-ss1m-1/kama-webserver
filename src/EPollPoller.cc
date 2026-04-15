#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <EPollPoller.h>
#include <Logger.h>
#include <Channel.h>

const int kNew = -1;    // 某个channel还没添加至Poller监听树 映射表中也没有    // channel的成员index_初始化为-1
const int kAdded = 1;   // 某个channel已经添加至Poller监听树
const int kDeleted = 2; // 某个channel已经从Poller监听树删除     // 并非真正删除 只是暂时禁用该channel对象 映射表中仍然保存其映射

EPollPoller::EPollPoller(EventLoop *loop)
    : Poller(loop)
    , epollfd_(::epoll_create1(EPOLL_CLOEXEC)) 
    , revents_(kInitEventListSize) // vector<epoll_event>(16)
{
    if (epollfd_ < 0)
    {
        LOG_FATAL<<"epoll_create error:%d \n"<<errno;
    }
}

EPollPoller::~EPollPoller()
{
    ::close(epollfd_);
}

Timestamp EPollPoller::poll(int timeoutMs, ChannelList *activeChannels)
{
    // 由于频繁调用poll 实际上应该用LOG_DEBUG输出日志更为合理 当遇到并发场景 关闭DEBUG日志提升效率
    LOG_INFO<<"fd total count:"<<channels_.size();

    int numEvents = ::epoll_wait(epollfd_, &*revents_.begin(), static_cast<int>(revents_.size()), timeoutMs);     // &*revents_.begin() 解引用(*)获取首元素 再取地址(&) -> 首元素的地址
    int saveErrno = errno;
    Timestamp now(Timestamp::now());

    if (numEvents > 0)
    {
        LOG_INFO<<"events happend"<<numEvents; // LOG_DEBUG最合理
        fillActiveChannels(numEvents, activeChannels);
        if (numEvents == revents_.size()) // 扩容操作
        {
            revents_.resize(revents_.size() * 2);
        }
    }
    else if (numEvents == 0)
    {
        LOG_DEBUG<<"timeout!";
    }
    else
    {
        if (saveErrno != EINTR)
        {
            errno = saveErrno;
            LOG_ERROR<<"EPollPoller::poll() error!";
        }
    }
    return now;
}

//状态机模式：updataChannle为决策者 update为执行者
// channel update remove => EventLoop updateChannel removeChannel => Poller updateChannel removeChannel
void EPollPoller::updateChannel(Channel *channel)
{
    const int index = channel->index();
    LOG_INFO<<"func =>"<<"fd"<<channel->fd()<<"events="<<channel->events()<<"index="<<index;

    if (index == kNew || index == kDeleted)
    {
        if (index == kNew)          //添加到映射表中 + 启用
        {
            int fd = channel->fd();
            channels_[fd] = channel;
        }
        else // index == kDeleted -> 重新启用该channel
        {
        }
        channel->set_index(kAdded);
        update(EPOLL_CTL_ADD, channel);
    }
    else // channel已经在Poller中注册过了
    {
        int fd = channel->fd();
        if (channel->isNoneEvent())         //无监听的事件 可删除
        {
            update(EPOLL_CTL_DEL, channel);
            channel->set_index(kDeleted);
        }
        else                                //更改了监听的事件
        {
            update(EPOLL_CTL_MOD, channel);
        }
    }
}

/*
从Poller中删除channel(真正删除 相应映射表的项也删除)
使用时机：连接关闭、错误恢复、服务器关闭清理资源？
*/
void EPollPoller::removeChannel(Channel *channel)
{
    int fd = channel->fd();
    channels_.erase(fd);

    LOG_INFO<<"removeChannel fd="<<fd;

    int index = channel->index();
    if (index == kAdded)        //只有真正注册了才删除 另外两种状态不需要删除
    {
        update(EPOLL_CTL_DEL, channel);
    }
    channel->set_index(kNew);   //重置为初始状态 防止后续误用   ---  防御性编程/channel池后续复用
}

// 填写活跃的连接(根据epollwait返回的事件列表->ptr->传出相应的channel列表)
void EPollPoller::fillActiveChannels(int numEvents, ChannelList *activeChannels) const
{
    for (int i = 0; i < numEvents; ++i)
    {
        Channel *channel = static_cast<Channel *>(revents_[i].data.ptr);
        channel->set_revents(revents_[i].events);
        activeChannels->push_back(channel); // EventLoop就拿到了它的Poller给它返回的所有发生事件的channel列表了
    }
}

// 更新channel通道 其实就是调用epoll_ctl add/mod/del
void EPollPoller::update(int operation, Channel *channel)
{
    epoll_event event;
    ::memset(&event, 0, sizeof(event));

    int fd = channel->fd();

    event.events = channel->events();
    event.data.fd = fd;
    event.data.ptr = channel;       //?:用于在事件触发时快速找到对应的channel对象(fillActiveChannels中使用)

    if (::epoll_ctl(epollfd_, operation, fd, &event) < 0)
    {
        if (operation == EPOLL_CTL_DEL)
        {
            LOG_ERROR<<"epoll_ctl del error:"<<errno;
        }
        else
        {
            LOG_FATAL<<"epoll_ctl add/mod error:"<<errno;
        }
    }
}