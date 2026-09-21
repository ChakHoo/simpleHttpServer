#include "server.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <assert.h>
#include <sys/sendfile.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>

struct FdInfo
{
    int fd;
    int epfd;
    pthread_t tid;
};

//返回监听套接字
int initListenFd(unsigned short port)
{
    //1.创建监听的fd
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if(lfd == -1)
    {
        perror("socket");
        return -1;
    }
    //2.设置端口复用
    int opt = 1; //启用端口复用选项
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret == -1)
    {
        perror("setsocket");
        return -1;
    }
    
    //3.绑定
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    ret = bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == -1)
    {
        perror("bind");
        return -1;
    }
    
    //4.设置监听
    ret = listen(lfd, 128);
    if (ret == -1)
    {
        perror("listen");
        return -1;
    }
    //返回监听fd
    return lfd;
}

int epollRun(int lfd)
{
    //1.创建epoll实例
    int epfd = epoll_create1(0); //epfd维护两类事件：1.我要监控哪些fd 2.当时哪些fd已就绪
    if (epfd == -1)
    {
        perror("epoll_creat");
        return -1;
    }
    //2.lfd上树
    struct epoll_event ev;
    ev.data.fd = lfd;
    ev.events = EPOLLIN;

    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1)
    {
        perror("epoll_ctl");
        return -1;
    }
    //3.检测
    struct epoll_event evs[1024];
    int size = sizeof(evs) / sizeof(evs[0]);
    while (1)
    {
        /*
            没有事件时，epoll_wait() 阻塞等待。
            有事件时，把事件放入 evs。
            返回值 num 表示本次返回了多少个事件。
            最后的 -1 表示不设置超时时间。
        */
        int num = epoll_wait(epfd, evs, size, -1);
        for (int i = 0; i < num; i++)
        {
            //每处理一个事件，都申请一个结构体。
            struct FdInfo* info = (struct FdInfo*)malloc(sizeof(struct FdInfo));
            int fd = evs[i].data.fd;
            info->epfd = epfd;
            info->fd = fd;
            if(fd == lfd) //监听套接字
            {
                //建立新链接accpet
                //acceptClient(lfd, epfd);
                pthread_create(&info->tid, NULL, acceptClient, info);
            }
            else //客户端套接字
            {
                //主要是接受对端数据
                //recvHttpRequest(fd, epfd);
                pthread_create(&info->tid, NULL, recvHttpRequest, info);
            }
        }
    }
    return 0;
}

//接收连接，把新客户端加入epoll
//int acceptClient(int lfd, int epfd)
void* acceptClient(void *arg)
{
    struct FdInfo* info = (struct FdInfo*)arg;
    //1.建立连接
    int cfd = accept(info->fd , NULL, NULL);
    if(cfd == -1)
    {
        perror("accpet");
        return NULL;
    }

    //2.设置非阻塞(read没有数据的时候立马返回-1)
    int flag = fcntl(cfd, F_GETFL); //获取当前文件描述符状态
    flag = flag | O_NONBLOCK; //加上“非阻塞”标志
    fcntl(cfd, F_SETFL, flag); //设置回去

    //3.cfd添加到epoll中
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET; //EPOLLET边沿触发模式

    int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1)
    {
        perror("epoll_ctl");
        return NULL;
    }
    printf("acceptclient threadId: %ld\n", info->tid);
    free(info); //释放传参结构体
    return NULL;
}

//读取客户端数据 提取请求行
//int recvHttpRequest(int cfd, int epfd)
void* recvHttpRequest(void* arg)
{
    struct FdInfo* info = (struct FdInfo*)arg;

    int len = 0, total = 0;
    char temp[1024] = {0};
    char buff[4096] = {0};
    while ((len = recv(info->fd, temp, sizeof(temp), 0))> 0)
    {
        if(total + len < sizeof(buff))
        {
            memcpy(buff + total, temp, len);
            total += len;
        }
    }

    if(len == -1 && errno == EAGAIN) //当前socket接收缓冲区里的数据已经被读空了，现在暂时没有数据可读。
    {
        //数据暂时接收完毕
        //解析请求行
        //strstr是用来在一个字符串中查找另一个字符串第一次出现的位置的函数。
        //strstr 的返回值是：指向“第一次匹配到的子字符串”的指针。
        char* pt = strstr(buff, "\r\n");
        int reqLen = pt - buff; //两个指针相减，即请求头长度
        buff[reqLen] = '\0'; //c字符串遇到\0结束
        parseRequestLine(buff, info->fd);
    }
    else if(len == 0) //客户端断开连接
    {
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
        close(info->fd);
    }
    else
    {
        perror("recv");
    }
    printf("recvMsg threadId: %ld\n", info->tid);
    free(info);
    return NULL;
}

//line:HTTP请求行
int parseRequestLine(const char* line, int cfd)
{
    // 解析请求行 GET /images/a.jpg HTTP/1.1
    char method[12]; //"GET"
    char path[1024]; //"/xxx/1/jpg"
    sscanf(line, "%[^ ] %[^ ]", method, path);//按照空格切
    printf("method: %s, path: %s\n", method, path);
    //只接受GET
    if (strcasecmp(method, "get") != 0)
    {
        return -1;
    }
    decodeMsg(path, path);///把类似的中文乱码还原Linux%E5%86%85%E6%A0%B8.jpg
    /*
        转化为本地相对路径
        path + 1 是为了跳过/images/a.jpg的/
        因为Linux 会把/理解成：从系统根目录 / 开始找
    */
    char* file = NULL;
    if (strcmp(path, "/") == 0)
    {
        file = "./";
    }
    else
    {
        file = path + 1;
    }
    // 获取文件属性
    struct stat st;
    int ret = stat(file, &st);
    if (ret == -1)
    {
        // 文件不存在 -- 回复404
        sendMsgHead(cfd, 404, "Not Found", getFileType(".html"), -1);
        sendFile("404.html", cfd);
        return 0;
    }
    // 判断文件类型
    if (S_ISDIR(st.st_mode))
    {
        // 把这个目录中的内容发送给客户端
        sendMsgHead(cfd, 200, "OK", getFileType(".html"), -1);
        sendDir(file, cfd);
    }
    else
    {
        // 把文件的内容发送给客户端
        sendMsgHead(cfd, 200, "OK", getFileType(file), st.st_size);
        sendFile(file, cfd);
    }

    return 0;
}

//发送文件
int sendFile(const char *fileName, int cfd)
{
    //1.打开文件
    int fd = open(fileName, O_RDONLY);
    if (fd == -1)
    {
        perror("open");
        return -1;
    }
#if 0    
    while (1)
    {
        char buff[1024];
        int len = read(fd, buff, sizeof(buff));
        if(len > 0)
        {
            send(cfd, buff, len, 0);
            usleep(10); //不要一直发数据，不然浏览器受不了
        }
        else if(len == 0)
        {
            break;
        }
        else 
        {
            perror("read");
        }
    }
#else
/*
    lseek 函数:
    作用：
    修改文件当前的读写位置，也可以用来获取文件大小。
    返回值：
    成功：返回移动后的文件偏移量
    失败：返回 -1
*/
    off_t offset = 0; //offset表示当前已经发送到文件的哪个位置
    int size = lseek(fd, 0, SEEK_END); //获取文件大小
    lseek(fd, 0, SEEK_SET);
    while (offset < size)
    {
        sendfile(cfd, fd, &offset, size - offset);
    }
#endif    
    close(fd); //关闭文件
    return 0;
}

/*
发送HTTP响应的开头
例如：HTTP/1.1 200 OK
     Content-Type: image/jpeg
     Content-Length: 50231
status:HTTP 状态码，例如 200、404
descr:状态描述，例如 "OK"、"Not Found"
type:文件类型，例如 "text/html"、"image/jpeg"
length:响应正文的字节数
*/
int sendMsgHead(int cfd, int status, const char *descr, const char* type, int length)
{
    //状态行
    char buff[4090] = {0};
    sprintf(buff, "HTTP/1.1 %d %s\r\n", status, descr);
    //响应头
    sprintf(buff + strlen(buff), "content-type: %s\r\n",type);
    sprintf(buff + strlen(buff), "content-length: %d\r\n\r\n", length);

    send(cfd, buff, strlen(buff), 0);
    return 0;
}

//根据扩展名告诉浏览器内容类型
const char* getFileType(const char* name)
{
    // a.jpg a.mp4 a.html
    // 自右向左查找‘.’字符, 如不存在返回NULL
    const char* dot = strrchr(name, '.');
    if (dot == NULL)
        return "text/plain; charset=utf-8";	// 纯文本
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";
}

//把目录内容变成一个网页
int sendDir(const char* dirName, int cfd)
{
    char buf[4096] = { 0 };
    sprintf(buf, "<html><head><title>%s</title></head><body><table>", dirName);
    struct dirent** namelist;
    int num = scandir(dirName, &namelist, NULL, alphasort);
    for (int i = 0; i < num; ++i)
    {
        // 取出文件名 namelist 指向的是一个指针数组 struct dirent* tmp[]
        char* name = namelist[i]->d_name;
        struct stat st;
        char subPath[1024] = { 0 };
        sprintf(subPath, "%s/%s", dirName, name);
        stat(subPath, &st);
        if (S_ISDIR(st.st_mode))
        {
            // a标签 <a href="">name</a>
            sprintf(buf + strlen(buf), 
                "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", 
                name, name, st.st_size);
        }
        else
        {
            sprintf(buf + strlen(buf),
                "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",
                name, name, st.st_size);
        }
        send(cfd, buf, strlen(buf), 0);
        memset(buf, 0, sizeof(buf));
        free(namelist[i]); //scandir会动态申请内存给每一个namelist[i]
    }
    sprintf(buf, "</table></body></html>");
    send(cfd, buf, strlen(buf), 0);
    free(namelist);
    return 0;
}

// 把一个十六进制字符转换成对应的十进制数值
int hexToDec(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

// 解码
// to 存储解码之后的数据, 传出参数, from被解码的数据, 传入参数
void decodeMsg(char* to, char* from)
{
    for (; *from != '\0'; ++to, ++from)
    {
        // isxdigit -> 判断字符是不是16进制格式, 取值在 0-f
        // Linux%E5%86%85%E6%A0%B8.jpg
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2]))
        {
            // 将16进制的数 -> 十进制 将这个数值赋值给了字符 int -> char
            // B2 == 178
            // 将3个字符, 变成了一个字符, 这个字符就是原始数据
            *to = hexToDec(from[1]) * 16 + hexToDec(from[2]);

            // 跳过 from[1] 和 from[2] 因此在当前循环中已经处理过了
            from += 2;
        }
        else
        {
            // 字符拷贝, 赋值
            *to = *from;
        }

    }
    *to = '\0';
}