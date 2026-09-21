#include <stdio.h>
#include <unistd.h>
#include "server.h"
#include <stdlib.h>

/*
假设编译后的程序叫 simpleHttp
启动程序: ./simpleHttp 8080 /home/user/www
此时参数对应关系:
变量          值                含义
argc	      3	         参数总数，包括程序名
argv[0]	 "./simpleHttp"	     程序名
argv[1]	    "8080"	       监听端口，以字符串形式传入
argv[2]	 "/home/user/www"   提供文件的目录
*/
int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        printf("./a.out port path\n");
        return -1;
    }
    unsigned short port = atoi(argv[1]);//atoi把字符串转化为整数
    //切换服务器的工作路径
    chdir(argv[2]); //切换工作目录为/home/user/www
    //初始化监听的套接字
    int lfd = initListenFd(port);
    // 启动服务器程序
    epollRun(lfd);
    
    return 0;
}
