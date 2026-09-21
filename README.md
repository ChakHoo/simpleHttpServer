# SimpleHttp

一个使用 **C 语言** 在 Linux 环境下实现的简易 HTTP 静态资源服务器。

项目基于 **Socket + epoll + 非阻塞 I/O + pthread 多线程** 实现，能够接收浏览器的 HTTP GET 请求，并根据请求路径返回目录页面、普通文件或 404 页面。该项目适合作为 Linux 网络编程、I/O 多路复用和 HTTP 协议入门练习。

---

## 1. 项目功能

当前实现的主要功能包括：

- TCP Socket 服务器
- `SO_REUSEADDR` 端口复用
- `epoll` I/O 多路复用
- 客户端 Socket 非阻塞
- `EPOLLET` 边沿触发模式
- `pthread` 多线程处理连接和请求
- HTTP GET 请求解析
- URL 百分号编码解码
- 静态文件传输
- 根据文件扩展名设置 `Content-Type`
- 动态生成目录浏览页面
- 文件不存在时返回 404 页面

---

## 2. 项目结构

```text
simpleHttp/
├── main.c          # 程序入口
├── server.c        # HTTP 服务器主要实现
├── server.h        # 函数声明
├── server          # 编译生成的可执行文件，不建议提交到 Git
└── www/            # 网站根目录
    ├── index.html
    ├── 404.html
    ├── hello.txt
    ├── images/
    └── ...
```

建议将 `server`、`*.o`、`core.*` 等编译产物加入 `.gitignore`。

---

## 3. 编译与运行

### 3.1 编译

在项目根目录执行：

```bash
gcc main.c server.c -o server -pthread
```

其中：

- `main.c`：程序入口
- `server.c`：服务器实现
- `-o server`：生成名为 `server` 的可执行文件
- `-pthread`：启用并链接 POSIX 线程库

### 3.2 运行

程序启动时需要传入两个参数：

```bash
./server 端口号 网站根目录
```

例如：

```bash
./server 8080 ./www
```

程序会将 `./www` 切换为当前工作目录，然后在 `8080` 端口监听客户端连接。

浏览器访问：

```text
http://localhost:8080/
```

或者直接访问某个文件：

```text
http://localhost:8080/index.html
```

---

## 4. 整体工作流程

服务器的核心执行流程如下：

```text
main()
  │
  ├── 读取端口号和网站根目录
  ├── chdir() 切换网站工作目录
  │
  ├── initListenFd()
  │      └── 创建并返回监听套接字 lfd
  │
  └── epollRun(lfd)
         │
         ├── 创建 epoll 实例
         ├── 将 lfd 加入 epoll
         │
         └── epoll_wait()
                │
                ├── lfd 就绪
                │      └── 创建线程
                │             └── acceptClient()
                │                    ├── accept()
                │                    ├── 设置 cfd 非阻塞
                │                    └── cfd 加入 epoll
                │
                └── cfd 就绪
                       └── 创建线程
                              └── recvHttpRequest()
                                     ├── recv() 读取 HTTP 请求
                                     ├── 提取请求行
                                     └── parseRequestLine()
                                            │
                                            ├── 文件不存在 → 404
                                            ├── 目录 → sendDir()
                                            └── 普通文件 → sendFile()
```

---

# 5. 核心函数说明

## 5.1 `main`

```c
int main(int argc, char* argv[])
```

`main()` 是程序入口，主要完成三个步骤：

1. 从命令行参数获取服务器端口号；
2. 使用 `chdir()` 将网站目录切换为当前工作目录；
3. 创建监听 Socket，并启动 epoll 事件循环。

例如：

```bash
./server 8080 ./www
```

对应：

```text
argv[0] = "./server"
argv[1] = "8080"
argv[2] = "./www"
```

其中：

```c
unsigned short port = atoi(argv[1]);
chdir(argv[2]);

int lfd = initListenFd(port);
epollRun(lfd);
```

---

## 5.2 `initListenFd`

```c
int initListenFd(unsigned short port);
```

作用：

> 根据指定端口创建 TCP 监听 Socket，并返回监听文件描述符 `lfd`。

主要流程：

```text
socket()
   ↓
setsockopt(SO_REUSEADDR)
   ↓
bind()
   ↓
listen()
   ↓
返回 lfd
```

### 主要步骤

创建 TCP Socket：

```c
int lfd = socket(AF_INET, SOCK_STREAM, 0);
```

开启端口复用：

```c
setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, ...);
```

绑定服务器地址：

```c
addr.sin_family = AF_INET;
addr.sin_port = htons(port);
addr.sin_addr.s_addr = INADDR_ANY;
```

开始监听：

```c
listen(lfd, 128);
```

最终返回：

```c
return lfd;
```

---

## 5.3 `epollRun`

```c
int epollRun(int lfd);
```

`epollRun()` 是服务器的事件调度核心。

首先创建 epoll 实例：

```c
int epfd = epoll_create1(0);
```

然后将监听 Socket `lfd` 加入 epoll：

```c
ev.data.fd = lfd;
ev.events = EPOLLIN;

epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
```

之后通过：

```c
epoll_wait(epfd, evs, size, -1);
```

持续等待事件。

服务器会根据发生事件的文件描述符进行分类：

### 情况一：监听 Socket `lfd` 就绪

说明：

> 有新的客户端正在请求建立 TCP 连接。

服务器创建线程执行：

```c
acceptClient(info);
```

### 情况二：客户端通信 Socket `cfd` 就绪

说明：

> 某个已经连接的客户端有数据可以读取。

服务器创建线程执行：

```c
recvHttpRequest(info);
```

线程参数使用：

```c
struct FdInfo
{
    int fd;
    int epfd;
    pthread_t tid;
};
```

其中：

- `fd`：当前需要处理的文件描述符
- `epfd`：epoll 实例
- `tid`：创建出的线程 ID

`FdInfo` 在堆区通过 `malloc()` 创建，因此线程处理结束后需要调用：

```c
free(info);
```

释放内存。

---

## 5.4 `acceptClient`

```c
void* acceptClient(void* arg);
```

该函数运行在新线程中，负责处理新的客户端连接。

### 1. 建立连接

```c
int cfd = accept(info->fd, NULL, NULL);
```

`accept()` 返回服务器端用于和该客户端通信的 Socket：

```text
lfd  → 只负责监听新连接
cfd  → 负责和某一个客户端通信
```

### 2. 设置非阻塞

```c
int flag = fcntl(cfd, F_GETFL);
flag |= O_NONBLOCK;
fcntl(cfd, F_SETFL, flag);
```

非阻塞模式下，如果当前没有数据可读，`recv()` 不会一直阻塞等待。

### 3. 加入 epoll

```c
ev.data.fd = cfd;
ev.events = EPOLLIN | EPOLLET;

epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
```

其中：

```text
EPOLLIN → 监听读事件
EPOLLET → 使用边沿触发模式
```

处理完成后释放线程参数：

```c
free(info);
```

---

## 5.5 `recvHttpRequest`

```c
void* recvHttpRequest(void* arg);
```

该函数负责读取客户端发送的 HTTP 请求。

因为客户端 Socket 使用了 **非阻塞 + EPOLLET**，所以收到一次读事件后，需要不断调用：

```c
recv()
```

直到当前 Socket 接收缓冲区中的数据全部读完。

```c
while ((len = recv(info->fd, temp, sizeof(temp), 0)) > 0)
{
    ...
}
```

每次接收到的数据先存入 `temp`，再拼接到完整请求缓冲区 `buff`。

当：

```c
len == -1 && errno == EAGAIN
```

表示：

> 当前 Socket 接收缓冲区已经被读空，目前暂时没有更多数据。

此时程序查找：

```text
\r\n
```

得到 HTTP 请求的第一行，并在末尾加入：

```c
'\0'
```

形成标准 C 字符串，然后交给：

```c
parseRequestLine(buff, info->fd);
```

进行进一步解析。

如果：

```c
len == 0
```

说明对端已经关闭 TCP 连接，此时：

```c
epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
close(info->fd);
```

将对应 Socket 从 epoll 中删除并关闭。

---

## 5.6 `parseRequestLine`

```c
int parseRequestLine(const char* line, int cfd);
```

这是 HTTP 请求处理的核心函数。

例如浏览器发送：

```http
GET /images/a.jpg HTTP/1.1
```

### 1. 解析请求行

使用：

```c
sscanf(line, "%[^ ] %[^ ]", method, path);
```

得到：

```text
method = "GET"
path   = "/images/a.jpg"
```

当前服务器只处理 GET 请求：

```c
if (strcasecmp(method, "get") != 0)
{
    return -1;
}
```

### 2. URL 解码

```c
decodeMsg(path, path);
```

例如：

```text
Linux%E5%86%85%E6%A0%B8.jpg
```

会被还原为原始文件名。

### 3. 转换为本地路径

如果访问：

```text
/
```

则：

```c
file = "./";
```

表示当前网站根目录。

如果访问：

```text
/images/a.jpg
```

则：

```c
file = path + 1;
```

跳过最前面的 `/`：

```text
/images/a.jpg
 ↓
images/a.jpg
```

这样得到的是相对于网站根目录的本地路径。

### 4. 判断文件是否存在

```c
struct stat st;
int ret = stat(file, &st);
```

如果：

```c
ret == -1
```

说明目标不存在，服务器返回：

```http
HTTP/1.1 404 Not Found
```

并发送 `404.html`。

### 5. 判断目录还是普通文件

如果：

```c
S_ISDIR(st.st_mode)
```

说明目标是目录：

```c
sendMsgHead(...);
sendDir(file, cfd);
```

如果不是目录，则按照普通文件处理：

```c
sendMsgHead(...);
sendFile(file, cfd);
```

---

## 5.7 `sendFile`

```c
int sendFile(const char* fileName, int cfd);
```

作用：

> 将服务器上的普通文件发送给浏览器。

首先打开文件：

```c
int fd = open(fileName, O_RDONLY);
```

然后使用：

```c
lseek(fd, 0, SEEK_END);
```

获取文件大小，并重新将文件偏移量移动到开头。

最后通过：

```c
sendfile(cfd, fd, &offset, size - offset);
```

将文件内容发送到客户端 Socket。

整体数据流：

```text
服务器磁盘文件
      │
      │ sendfile()
      ↓
客户端 Socket cfd
      │
      ↓
浏览器
```

相比传统的：

```text
read() → 用户空间缓冲区 → send()
```

当前实现直接使用 `sendfile()` 完成文件发送。

---

## 5.8 `sendMsgHead`

```c
int sendMsgHead(
    int cfd,
    int status,
    const char* descr,
    const char* type,
    int length
);
```

作用：

> 生成并发送 HTTP 响应头。

例如发送 JPG 图片时，可以生成：

```http
HTTP/1.1 200 OK
content-type: image/jpeg
content-length: 50231

```

其中：

- `status`：HTTP 状态码，例如 `200`、`404`
- `descr`：状态描述，例如 `OK`、`Not Found`
- `type`：资源 MIME 类型
- `length`：响应正文长度

响应头通过：

```c
send(cfd, buff, strlen(buff), 0);
```

发送给浏览器。

---

## 5.9 `getFileType`

```c
const char* getFileType(const char* name);
```

作用：

> 根据文件扩展名得到对应的 HTTP `Content-Type`。

例如：

| 文件 | Content-Type |
|---|---|
| `.html` | `text/html; charset=utf-8` |
| `.jpg` / `.jpeg` | `image/jpeg` |
| `.png` | `image/png` |
| `.css` | `text/css` |
| `.mp3` | `audio/mpeg` |
| `.wav` | `audio/wav` |
| 其他未知类型 | `text/plain; charset=utf-8` |

浏览器会根据 `Content-Type` 判断应该如何解释收到的数据。

例如：

```http
Content-Type: image/jpeg
```

表示响应正文是一张 JPEG 图片。

---

## 5.10 `sendDir`

```c
int sendDir(const char* dirName, int cfd);
```

作用：

> 扫描服务器目录，将目录中的文件和子目录动态生成一个 HTML 页面，再发送给浏览器。

### 1. 生成 HTML 开头

```html
<html>
<head>
    <title>./</title>
</head>
<body>
<table>
```

### 2. 扫描目录

使用：

```c
scandir(dirName, &namelist, NULL, alphasort);
```

读取目录中的所有条目，并按名称排序。

通过：

```c
namelist[i]->d_name
```

取得当前文件或目录名称。

### 3. 获取文件信息

程序组合出完整路径：

```c
sprintf(subPath, "%s/%s", dirName, name);
```

然后：

```c
stat(subPath, &st);
```

判断该条目是目录还是普通文件。

### 4. 目录条目

如果是目录，会生成类似：

```html
<tr>
    <td>
        <a href="images/">images</a>
    </td>
    <td>4096</td>
</tr>
```

目录链接末尾带 `/`，点击后浏览器会继续访问该目录。

### 5. 普通文件

如果是普通文件，会生成：

```html
<tr>
    <td>
        <a href="a.jpg">a.jpg</a>
    </td>
    <td>50231</td>
</tr>
```

点击文件名以后，浏览器会再次发送 GET 请求，服务器最终调用 `sendFile()` 返回文件。

### 6. 发送 HTML

每生成一部分 HTML，就通过：

```c
send(cfd, buf, strlen(buf), 0);
```

发送给浏览器。

最后发送：

```html
</table>
</body>
</html>
```

完成整个目录页面。

`scandir()` 会动态申请内存，因此每个：

```c
namelist[i]
```

使用结束后都需要：

```c
free(namelist[i]);
```

最后再：

```c
free(namelist);
```

释放整个指针数组。

---

## 5.11 `decodeMsg` 与 `hexToDec`

```c
void decodeMsg(char* to, char* from);
int hexToDec(char c);
```

这两个函数用于处理 URL 百分号编码。

例如浏览器请求路径可能包含：

```text
%E5%86%85%E6%A0%B8
```

`decodeMsg()` 会识别：

```text
%xx
```

形式的数据，再通过：

```c
hexToDec()
```

把两个十六进制字符转换成对应的字节。

例如：

```text
%20
```

会被还原成空格。

---

# 6. 一次完整 HTTP 请求示例

假设浏览器访问：

```text
http://localhost:8080/images/a.jpg
```

浏览器发送：

```http
GET /images/a.jpg HTTP/1.1
```

服务器处理流程：

```text
浏览器
   │
   │ TCP连接
   ↓
acceptClient()
   │
   └── cfd 加入 epoll
           │
           ↓
recvHttpRequest()
           │
           └── 读取 GET /images/a.jpg HTTP/1.1
                    │
                    ↓
             parseRequestLine()
                    │
                    ├── method = GET
                    ├── path = /images/a.jpg
                    ├── 转成本地路径 images/a.jpg
                    └── stat()
                           │
                           ↓
                       普通文件
                           │
             ┌─────────────┴─────────────┐
             ↓                           ↓
      sendMsgHead()                 sendFile()
             │                           │
             ↓                           ↓
       HTTP响应头                    图片内容
             └─────────────┬─────────────┘
                           ↓
                         浏览器
```

最终浏览器收到：

```http
HTTP/1.1 200 OK
Content-Type: image/jpeg
Content-Length: 50231

<JPEG 二进制数据>
```

---

# 7. 核心技术点

## epoll

项目通过 `epoll` 同时监听：

- 监听 Socket `lfd`
- 多个客户端通信 Socket `cfd`

避免为每个客户端单独阻塞等待数据。

## EPOLLET

客户端 Socket 使用：

```c
EPOLLIN | EPOLLET
```

即边沿触发模式。

因此收到事件后需要持续 `recv()`，直到：

```c
errno == EAGAIN
```

表示当前已经把 Socket 接收缓冲区读空。

## 非阻塞 I/O

客户端 Socket 使用：

```c
O_NONBLOCK
```

避免 `recv()` 在没有数据时一直阻塞线程。

## pthread

当前实现中：

- 新客户端连接事件 → 创建线程执行 `acceptClient`
- 客户端读事件 → 创建线程执行 `recvHttpRequest`

从而将 epoll 的事件检测和具体业务处理分开。

---

# 8. 当前版本的已知限制

本项目主要用于 Linux 网络编程与 HTTP 协议学习，目前仍属于简化版 HTTP Server，例如：

- 当前只处理 GET 请求；
- HTTP 请求解析只重点处理请求行；
- 未实现完整的 HTTP Keep-Alive 管理；
- 未实现 POST、PUT 等请求方法；
- 未实现完整的异常连接和半关闭处理；
- 每次事件都会创建新线程，尚未使用线程池；
- 部分发送逻辑还可以进一步补充非阻塞状态下的 `EAGAIN` 处理；
- 目录和 404 响应的 `Content-Length` 还可以进一步完善。

后续可以继续改进为：

```text
epoll + 非阻塞 I/O + 线程池 + 完整 HTTP 请求解析
```

进一步提升服务器的并发处理能力和健壮性。

---

# 9. 学习目标

通过该项目可以练习和理解：

- Linux 文件描述符
- Socket 网络编程
- TCP 服务器基本流程
- `accept / recv / send`
- `fcntl` 非阻塞设置
- `epoll`
- LT 与 ET 模式
- `pthread`
- HTTP 请求与响应格式
- `stat`
- `scandir`
- `sendfile`
- URL 编码与解码
- 动态内存管理

---

## License

This project is for learning and educational purposes.
