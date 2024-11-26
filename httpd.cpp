#include <iostream>
#include <errno.h>
#include <assert.h>

#include <stdio.h>
#include <string.h>
#include "httpd.h"
#include <sys/sendfile.h>

#define DEBUG_MODE 0
std::mutex debug_cout_mtx;

ThreadPool::ThreadPool(unsigned int threads_num) {
    quit = false;
    if(threads_num <= 0 || threads_num > std::thread::hardware_concurrency())
    {
        // 如果输入的线程数小于等于0或者大于可以运行的最大线程数,则设置线程数为当前环境下可用的最大线程数
        threads_num = std::thread::hardware_concurrency();
    }
    for(int i=0;i<static_cast<int>(threads_num);++i)
    {
        _workers.emplace_back([this](){
            while(true)
            {
                std::unique_lock<std::mutex> _g(_mtx);
                _cv.wait(_g, [&](){
                    // 要么接收到退出消息，要么收到新的任务
                    return (quit || (!_tasks.empty()));
                });
                if(quit && _tasks.empty())
                {
                    // 线程池退出
                    return;
                }

                auto task = _tasks.front();
                _tasks.pop();
                task();     // 执行任务
            }
        });
    }
}

ThreadPool::~ThreadPool(){
    {
        std::lock_guard<std::mutex> _g(_mtx);
        quit = true;
    }
    // 通知所有线程要求退出
    _cv.notify_all();
    for(auto &t : _workers)
    {
        if(t.joinable())
        {
            t.join();
        }
    }
}

void set_nonblocking_mode(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void snd_403(int clnt_sock)
{
    char res_header[BUF_SIZE];
    snprintf(res_header, sizeof(res_header), 
            "HTTP/1.1 403 Forbidden\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 0\r\n"
            "\r\n"
        );
    send(clnt_sock, res_header, strlen(res_header), 0);
}

void snd_404(int clnt_sock)
{
    char res_header[BUF_SIZE];
    snprintf(res_header, sizeof(res_header), 
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 0\r\n"
            "\r\n"
        );
    send(clnt_sock, res_header, strlen(res_header), 0);
}

int snd_files(int clnt_sock, FILE_TYPES ftypes, const char *file_path)
{
    // ssize_t bytes_read;
    char res_header[BUF_SIZE];
    // char buffer[BUF_SIZE];

    FILE *fp = fopen(file_path, "rb");
    if(fp == nullptr)
    {
#if DEBUG_MODE
        std::unique_lock<std::mutex> _g(debug_cout_mtx);
        std::cerr << "fopen " << file_path << " error!\r\n";
#endif
        return -1;
    }

    // 计算文件大小
    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // 制定HTTP回应头
    switch (ftypes) {
        case HTML_FILE: {
            snprintf(res_header, sizeof(res_header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     file_size);
            break;
        }
        case CSS_FILE: {
            snprintf(res_header, sizeof(res_header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/css\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     file_size);
            break;
        }
        case JS_FILE: {
            snprintf(res_header, sizeof(res_header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/javascript\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     file_size);
            break;
        }
        case JPEG_FILE: {
            snprintf(res_header, sizeof(res_header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: image/jpeg\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     file_size);
            break;
        }
        case PNG_FILE: {
            snprintf(res_header, sizeof(res_header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: image/png\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     file_size);
            break;
        }
    }

    // 发送HTTP头
    send(clnt_sock, res_header, strlen(res_header), 0);

    // 发送文件内容
    off_t offset = 0;
    ssize_t bytes_snd = sendfile(clnt_sock, fileno(fp), &offset, file_size);
    if(bytes_snd == -1)
    {
        return -1;
    }

#if DEBUG_MODE
    std::unique_lock<std::mutex> _g(debug_cout_mtx);
    std::cout << file_path << " write ok!\r\n";
#endif
    return 0;
}

void response_to_client(int fd, int epfd, const std::string doc_root)
{
    char buf[BUF_SIZE];
    int len;
    while(true){
        // * 边缘触发的epoll采用非阻塞式的socket套接字
        len = read(fd, buf, BUF_SIZE-1);
        if(len == 0)
        {
            break;
        } else if(len < 0)
        {
            if(errno == EAGAIN)
            {
                break;
            }
        }
    }
#if DEBUG_MODE
    {
        std::unique_lock<std::mutex> _g(debug_cout_mtx);
        std::cout << "buf=" << buf << "\r\n";
    }
#endif
    if(strlen(buf) == 0)
    {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        return;
    }
    assert(strlen(buf) != 0);
    // 解析http请求:
    char *get = strtok(buf, " "); // "GET"
    if(strcmp(get, "GET"))
    {
        snd_403(fd);
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        return;
    }
    char *request = strtok(NULL, " ");      // 请求的文件

#if DEBUG_MODE
    {
        std::unique_lock<std::mutex> _g(debug_cout_mtx);
        std::cout << "initial request: " << request << "\r\n";
    }
#endif
    // TODO: 需要解析该原始请求里是否有?以及键值对
    bool stupid_download_suffix = false;
    char *last_dot = strrchr(request, '.');
    if(last_dot != NULL)
    {
        ++last_dot;
        stupid_download_suffix = true;
    }
    char sub_str[BUF_SIZE];
    memset(sub_str, 0, BUF_SIZE);
    // NOTE: add last_dot != nullptr
    if(stupid_download_suffix && last_dot != nullptr && strstr(last_dot, "%")!=NULL)
    {
        // NOTE: 有个别js文件以".下载"结尾(居然要为这么个".下载"文件弄一个处理,我认为这是一个很愚蠢的任务!!!!)
        char tmp_str[BUF_SIZE];
        strncpy(tmp_str, request, last_dot-request);
        tmp_str[last_dot-request] = '\0';
        strcat(tmp_str, "下载");
        strcat(sub_str, doc_root.c_str());
        strcat(sub_str, tmp_str);

#if DEBUG_MODE
        std::unique_lock<std::mutex> _g(debug_cout_mtx);
        std::cout << "tmp_str = " << tmp_str << "\r\n";
        std::cout << "doc_root = " << doc_root << "\r\n"; 
        std::cout << "sub_str=" << sub_str << "\r\n";
#endif
    } else {
        strcat(sub_str, doc_root.c_str());
        strcat(sub_str, request);
#if DEBUG_MODE
        std::unique_lock<std::mutex> _g(debug_cout_mtx);
        std::cout << "sub_str=" << sub_str << "\r\n";
#endif
    }

    // TODO: 可以使用正则表达式来处理
    if(strstr(sub_str, ".html") != NULL)
    {
        if(snd_files(fd,HTML_FILE, sub_str) == -1)
        {
            snd_404(fd);
        }
    }
    else if(strstr(sub_str, ".css") != NULL)
    {
        if(snd_files(fd, CSS_FILE, sub_str) == -1)
        {
            snd_404(fd);
        }
    } else if(strstr(sub_str, ".js") != NULL)
    {
        if(snd_files(fd, JS_FILE, sub_str) == -1)
        {
            snd_404(fd);
        }
    } 
    else if(strstr(sub_str, ".png") != NULL){
        if(snd_files(fd, PNG_FILE, sub_str) == -1){
            snd_404(fd);
        }
    } else if(strstr(sub_str, ".jpg") != NULL)
    {
        if(snd_files(fd, JPEG_FILE, sub_str) == -1)
        {
            snd_404(fd);
        }
    }
    else {
        snd_404(fd);
    }

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

void start_httpd(unsigned short port, std::string doc_root)
{
    int res;
    int serv_sock, clnt_sock;
    struct sockaddr_in serv_addr, clnt_addr;
    socklen_t clnt_addr_sz = sizeof(clnt_addr);	

    struct epoll_event *ep_events;
    struct epoll_event event;
    int epfd, event_cnt;

    ThreadPool thread_pool;

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if(serv_sock == -1)
    {
        throw std::runtime_error("socket() error!");
        return;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    res = bind(serv_sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if(res == -1)
    {
        throw std::runtime_error("bind() error!");
        return;
    }
    res = listen(serv_sock, LISTEN_CNT);
    if(res == -1)
    {
        throw std::runtime_error("listen() error!");
        return;
    }

    epfd = epoll_create(EPOLL_SIZE);
    ep_events = (epoll_event*)malloc(sizeof(struct epoll_event) * EPOLL_SIZE);

    set_nonblocking_mode(serv_sock);
    event.events = EPOLLIN;
    event.data.fd = serv_sock;
    epoll_ctl(epfd, EPOLL_CTL_ADD, serv_sock, &event);

    while(true)
    {
        event_cnt = epoll_wait(epfd, ep_events, EPOLL_SIZE, -1);
        if(event_cnt == -1)
        {
            throw std::runtime_error("epoll_wait() error!");
            break;
        }

        for(int i=0;i<event_cnt;++i)
        {
            if(ep_events[i].data.fd == serv_sock) {
                // 有新的客户端连接
                clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_addr, &clnt_addr_sz);
                set_nonblocking_mode(clnt_sock);
                event.events = EPOLLIN | EPOLLET;
                event.data.fd = clnt_sock;
                epoll_ctl(epfd, EPOLL_CTL_ADD, clnt_sock, &event);
            } else {
                // 处理客户端发来的数据
                int clnt_fd = ep_events[i].data.fd;
                thread_pool.enqueue(response_to_client, clnt_fd, epfd, doc_root);
            }
        }
    }
    
}
