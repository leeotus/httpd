/**========================================================================
 * *                          ABOUT THIS PROJECT
 * @brief 高级计算机网络作业-httpd
 * @version 1.1.0
 * @date 2024/11/25
 * @author 202434561038 李建锨
 * @usage ./httpd <port> ./htdocs
 * @finish 
 *      1. 基础功能,能正常访问blog.html及其各个子页面,有些页面的图片是没有的,对于这些
 *      没有的资源,可以在浏览器中按F12看到发送的HTTP响应为404
 *      2. 正常访问jnu.html,同样有些资源是没有的,F12查看network可以看到服务器端发送
 *      的404码
 *      3. 对于输入错误的地址,服务器端默认返回404
 *      4. extensions, 这里仅完成了第3(采用C++11多线程池化技术,见下面ThreadPool类)
 *      和第4(采用epoll+非阻塞socket套接字:见set_nonblocking_mode函数和epoll的
 *      相关设置)
 * @note 因为不知道添加新的文件进来改项目是否会导致破坏项目结构而无法提交作业，
 * 所以将一些类(如线程池类)都放在了同一个头文件中
 * @test 测试发现该程序在edge浏览器中可以正常运行
 * @todo 解析带有键值对的URL请求
 *========================================================================**/

#pragma once

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <vector>
#include <queue>
#include <memory>
#include <unordered_map>

#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define LISTEN_CNT 20

constexpr int BUF_SIZE = 1024;
constexpr int EPOLL_SIZE = 2048;
constexpr int PARAM_SIZE = 128;

/**
 * @brief 不同类型的文件
 * @note 不同的文件有不同的HTTP响应头
 */
enum FILE_TYPES {
    HTML_FILE,      // html文件
    CSS_FILE,       // css文件
    JS_FILE,        // js文件
    JPEG_FILE,      // jpg文件
    PNG_FILE        // png文件
};

/**
 * @brief 线程池类
 */
class ThreadPool {
public:
    /**
     * @brief Construct a new Thread Pool object
     * @param threads_num 线程数量,默认情况下开启线程数目为std::thread::hardware_concurrency()
     */
    inline ThreadPool(unsigned int threads_num = 0);

    // 删除拷贝构造函数和复制函数
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief 向线程池的任务队列中传递任务函数和函数参数
     * @tparam F 函数类
     * @tparam Args 参数类
     * @param f 函数指针
     * @param args 要传入f函数的参数
     * @return std::future<typename std::result_of<F(Args...)>::type> 最后由future获取返回值
     */
    template<typename F, typename ...Args>
    auto enqueue(F&& f, Args&& ...args) -> std::future<typename std::result_of<F(Args...)>::type>;

    ~ThreadPool();

    inline void showQueueSize() {
        std::cout << _tasks.size() << "\r\n";
    }
private:
    bool quit;
    std::mutex _mtx;
    std::condition_variable _cv;
    std::vector<std::thread> _workers;
    std::queue<std::function<void()>> _tasks;
    
};

/**
 * @brief 为epoll边缘触发而设置非阻塞模式
 * @param fd 要设置非阻塞的文件描述符
 */
void set_nonblocking_mode(int fd);

/**
 * @brief 发送403 Forbidden码
 * @param clnt_sock 
 */
void snd_403(int clnt_sock);

/**
 * @brief 文件若请求失败,返回404码
 * @param clnt_sock 客户端套接字
 */
void snd_404(int clnt_sock);

/**
 * @brief 请求成功(200 OK)之后发送文件到浏览器(客户端)中
 * @param clnt_sock 客户端套接字 
 * @param file_path 指定要发送的文件
 * @return int 返回0表示成功,返回-1表示失败
 * @note 由上一次作业修改而来
 */
int snd_files(int clnt_sock, FILE_TYPES ftypes, const char *file_path);

/**
 * @brief 响应浏览器(客户端)的请求,发送对应的文件给客户端
 * @param fd 客户端文件描述符 
 * @param epfd epoll 文件描述符
 * @param doc_root 主目录
 */
void response_to_client(int fd, int epfd, const std::string doc_root);


/**
 * @brief http服务程序入口函数
 * @param port 监听端口
 * @param doc_root 文件路径
 */
void start_httpd(unsigned short port, std::string doc_root);

template <typename F, typename ...Args>
auto ThreadPool::enqueue(F&& f, Args&& ...args) -> std::future<typename std::result_of<F(Args...)>::type>
{
    using returnType = typename std::result_of<F(Args...)>::type;

    auto task = std::make_shared<std::packaged_task<returnType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    auto res = task->get_future();
    {
        std::unique_lock<std::mutex> _g(_mtx);
        if(quit) {
            throw std::runtime_error("enqueue on stopped ThreadPool!");
        }
        _tasks.emplace([task](){
            (*task)();
        });
    }
    _cv.notify_one();
    return res;
}
