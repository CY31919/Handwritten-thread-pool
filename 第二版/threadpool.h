#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <unordered_map>
#include <future>
#include <iostream>

const int TASK_MAX_THREADHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 10; // 单位是秒
// 线程池可以支持的模式
enum class PoolMood
{
    MOOD_FIXED,  // 固定数量的线程
    MOOD_CACHED, // 线程数量可动态增长
};
// 线程类型
class Thread
{
public:
    // 线程函数对象类型
    using ThreadFunc = std::function<void(int)>;

    // 线程构造
    Thread(ThreadFunc func) : func_(func), threadId_(generateId_++)
    {
    }

    // 线程析构
    ~Thread() = default;
    // 启动线程
    void start()
    {
        // 创建一个线程来执行一个线程函数
        std::thread t(func_, threadId_); // C++来说 线程对象t 和 线程函数 func_
        t.detach();                      // 设置分离线程
    }
    // 获取线程id
    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_; // 保存线程id
};

int Thread::generateId_ = 0;

// 线程池类型
class ThreadPool
{
public:
    // 线程池构造
    ThreadPool() : initThreadSize_(0), taskSize_(0), threadSizeThreshHold_(THREAD_MAX_THRESHHOLD), curThreadSize_(0),
        taskQueMaxThreadHold_(TASK_MAX_THREADHOLD), poolMood_(PoolMood::MOOD_FIXED), isPoolRunning_(false), idleThreadSize_(0)
    {
    }

    // 线程池析构
    ~ThreadPool()
    {
        isPoolRunning_ = false;
        // notEmpty_.notify_all();
        //  等待线程池所有的线程返回，有两种状态：阻塞 & 正在执行任务中
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]() -> bool
            { return threads_.size() == 0; });
    }

    // 设置线程池的工作模式
    void setMood(PoolMood mode)
    {
        if (checkRunningState())
            return;
        poolMood_ = mode;
    }

    // 设置task任务队列上限的阈值
    void setTaskQueMaxThreadHold(int threadhold)
    {
        taskQueMaxThreadHold_ = threadhold;
    }

    // 设置线程池cached模式下的线程阈值
    void setThreadSizeThreshHold(int threadhold)
    {
        if (checkRunningState())
            return;

        if (poolMood_ == PoolMood::MOOD_CACHED)
        {
            threadSizeThreshHold_ = threadhold;
        }
    }

    // 给线程池提交任务  这里使用可变参模板编程 让submitTask可以接收任意任务函数和任意数量的参数
    // Result submitTask(std::shared_ptr<Task> sp);
    // decltype 推导类型
    template <typename Func, typename... Args>
    auto submitTask(Func&& func, Args &&...args) -> std::future<decltype(func(args...))>
    {
        // 打包任务，放入队列
        using Rtype = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<Rtype()>>(std::bind(std::forward<Func>(func), forward<Args>(args)...));
        std::future<Rtype> result = task->get_future();

        // 获取锁
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        // 用户提交任务，最长不能阻塞超过一秒，否则判断提交任务失败，返回
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&]() -> bool
            { return taskQue_.size() < (size_t)taskQueMaxThreadHold_; })) // 满足条件继续向下运行
        {
            // 表示notFull等待1s钟，条件依然没有满足
            std::cerr << "task queue is full,submit task fail" << std::endl;
            auto task = std::make_shared<std::packaged_task<Rtype()>>([]()->Rtype {return Rtype(); });
            (*task)();
            return task->get_future();
        }
        // 如果有空余 把任务放入任务队列中
        // taskQue_.emplace(sp);
        taskQue_.emplace([task]() {
            // 去执行下面的任务
            (*task)();
            });
        taskSize_++;

        // 因为放入了任务，任务队列肯定不空了，notEmpty_通知,赶快分配线程执行任务
        notEmpty_.notify_all();

        // cache模式 任务处理比较紧急 场景：小而快的任务 需要根据任务数量和空闲线程的数量，判断是否需要创建新的线程出来
        if (poolMood_ == PoolMood::MOOD_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
        {
            std::cout << "create new thread... " << std::endl;
            // 创建新线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // threads_.emplace_back(std::move(ptr));
            threads_[threadId]->start();
            // 修改线程个数相关的变量
            curThreadSize_++;
            idleThreadSize_++;
        }

        // 返回任务的Result对象
        return result;
    }

    // 开启线程池
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        // 设置线程池的运行状态
        isPoolRunning_ = true;
        // 记录初始化线程个数
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        // 创建线程对象
        for (int i = 0; i < initThreadSize_; i++)
        {
            // 创建thread线程对象的时候，把线程函数给到thread线程对象
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // threads_.emplace_back(std::move(ptr));
        }
        // 启动所有线程 std::vector<Thread *> threads_;
        for (int i = 0; i < initThreadSize_; i++)
        {
            threads_[i]->start(); // 需要去执行一个线程函数
            idleThreadSize_++;    // 记录初始空闲  线程的数量
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // 定义线程函数
    void threadFunc(int threadid)
    {
        auto lastTime = std::chrono::high_resolution_clock().now();
        for (;;)
        {
            Task task;
            {
                // 获取锁
                std::unique_lock<std::mutex> lock(taskQueMtx_);
                std::cout << "tid" << std::this_thread::get_id() << " 尝试获取任务... " << std::endl;
                // cache模式下，有可能创建了很多的线程，但是空闲时间超过60s，应该把多余线程回收掉 需要知道当前时间和上一次执行的时间 > 60s

                // 每一秒钟返回一次 怎么区分：超时返回？还是有任务待执行返回
                // 锁+双重判断
                while (taskQue_.size() == 0)
                {
                    // 线程池要结束 回收线程资源
                    if (!isPoolRunning_)
                    {
                        threads_.erase(threadid);
                        std::cout << "threadid: " << std::this_thread::get_id() << "exit" << std::endl;
                        exitCond_.notify_all();
                        return; // 线程函数结束 线程结束
                    }
                    if (poolMood_ == PoolMood::MOOD_CACHED)
                    {
                        // 条件变量超时返回了
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                        {
                            auto nowTime = std::chrono::high_resolution_clock().now();
                            auto dur = std::chrono::duration_cast<std::chrono::seconds>(nowTime - lastTime);
                            if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                            {
                                threads_.erase(threadid);
                                curThreadSize_--;
                                idleThreadSize_--;

                                std::cout << "threadid: " << std::this_thread::get_id() << "exit" << std::endl;
                                return;
                            }
                        }
                    }
                    else
                    {
                        // 等待notEmpty条件
                        notEmpty_.wait(lock); // 满足是继续向下执行
                    }
                }

                idleThreadSize_--;
                std::cout << "tid" << std::this_thread::get_id() << " 获取任务成功... " << std::endl;
                // 从任务队列中取一个任务出来
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;
                // 如果依然有剩余任务，继续通知其他的线程执行任务
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }
                // 取出一个任务进行通知，通过可以继续提交生产任务
                notFull_.notify_all();
            } // 就应该把锁释放掉
            // 当前线程负责执行这个任务
            if (task != nullptr)
            {
                task();  // 执行function<void()>
            }
            idleThreadSize_++;
            lastTime = std::chrono::high_resolution_clock().now(); // 更新线程执行完任务的时间
        }
    }

    // 检查pool的运行状态
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表
    size_t initThreadSize_;                                    // 初始的线程数量
    std::atomic_int curThreadSize_;                            // 记录当前线程池里面的线程的总数量
    int threadSizeThreshHold_;                                 // 线程数量的上限阈值
    std::atomic_int idleThreadSize_;                           // 记录空闲线程的数量

    // Task任务 =》函数对象
    using Task = std::function<void()>;
    std::queue<Task> taskQue_; // 任务队列
    std::atomic_int taskSize_; // 任务的数量
    int taskQueMaxThreadHold_; // 任务队列上的阈值

    std::mutex taskQueMtx_;            // 保证任务队列的线程安全
    std::condition_variable notFull_;  // 表示任务队列不满
    std::condition_variable notEmpty_; // 表示任务队列不空
    std::condition_variable exitCond_; // 等待线程资源回收

    PoolMood poolMood_;              // 当前线程池的工作模式
    std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态
};

#endif