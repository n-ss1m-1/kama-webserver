#include <Poller.h>
#include <Channel.h>

Poller::Poller(EventLoop *loop)
    : ownerLoop_(loop)
{
}

bool Poller::hasChannel(Channel *channel) const
{
    auto it = channels_.find(channel->fd());
    return it != channels_.end() && it->second == channel;  //第二个条件：避免当连接关闭时 忘记从映射表中删除相应fd 而fd重用 导致fd指向悬空指针
}