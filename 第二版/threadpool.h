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
const int THREAD_MAX_IDLE_TIME = 10; // ��λ����
// �̳߳ؿ���֧�ֵ�ģʽ
enum class PoolMood
{
    MOOD_FIXED,  // �̶��������߳�
    MOOD_CACHED, // �߳������ɶ�̬����
};
// �߳�����
class Thread
{
public:
    // �̺߳�����������
    using ThreadFunc = std::function<void(int)>;

    // �̹߳���
    Thread(ThreadFunc func) : func_(func), threadId_(generateId_++)
    {
    }

    // �߳�����
    ~Thread() = default;
    // �����߳�
    void start()
    {
        // ����һ���߳���ִ��һ���̺߳���
        std::thread t(func_, threadId_); // C++��˵ �̶߳���t �� �̺߳��� func_
        t.detach();                      // ���÷����߳�
    }
    // ��ȡ�߳�id
    int getId() const
    {
        return threadId_;
    }

private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_; // �����߳�id
};

int Thread::generateId_ = 0;

// �̳߳�����
class ThreadPool
{
public:
    // �̳߳ع���
    ThreadPool() : initThreadSize_(0), taskSize_(0), threadSizeThreshHold_(THREAD_MAX_THRESHHOLD), curThreadSize_(0),
        taskQueMaxThreadHold_(TASK_MAX_THREADHOLD), poolMood_(PoolMood::MOOD_FIXED), isPoolRunning_(false), idleThreadSize_(0)
    {
    }

    // �̳߳�����
    ~ThreadPool()
    {
        isPoolRunning_ = false;
        // notEmpty_.notify_all();
        //  �ȴ��̳߳����е��̷߳��أ�������״̬������ & ����ִ��������
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]() -> bool
            { return threads_.size() == 0; });
    }

    // �����̳߳صĹ���ģʽ
    void setMood(PoolMood mode)
    {
        if (checkRunningState())
            return;
        poolMood_ = mode;
    }

    // ����task����������޵���ֵ
    void setTaskQueMaxThreadHold(int threadhold)
    {
        taskQueMaxThreadHold_ = threadhold;
    }

    // �����̳߳�cachedģʽ�µ��߳���ֵ
    void setThreadSizeThreshHold(int threadhold)
    {
        if (checkRunningState())
            return;

        if (poolMood_ == PoolMood::MOOD_CACHED)
        {
            threadSizeThreshHold_ = threadhold;
        }
    }

    // ���̳߳��ύ����  ����ʹ�ÿɱ��ģ���� ��submitTask���Խ������������������������Ĳ���
    // Result submitTask(std::shared_ptr<Task> sp);
    // decltype �Ƶ�����
    template <typename Func, typename... Args>
    auto submitTask(Func&& func, Args &&...args) -> std::future<decltype(func(args...))>
    {
        // ������񣬷������
        using Rtype = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<Rtype()>>(std::bind(std::forward<Func>(func), forward<Args>(args)...));
        std::future<Rtype> result = task->get_future();

        // ��ȡ��
        std::unique_lock<std::mutex> lock(taskQueMtx_);
        // �û��ύ�����������������һ�룬�����ж��ύ����ʧ�ܣ�����
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&]() -> bool
            { return taskQue_.size() < (size_t)taskQueMaxThreadHold_; })) // ��������������������
        {
            // ��ʾnotFull�ȴ�1s�ӣ�������Ȼû������
            std::cerr << "task queue is full,submit task fail" << std::endl;
            auto task = std::make_shared<std::packaged_task<Rtype()>>([]()->Rtype {return Rtype(); });
            (*task)();
            return task->get_future();
        }
        // ����п��� ������������������
        // taskQue_.emplace(sp);
        taskQue_.emplace([task]() {
            // ȥִ�����������
            (*task)();
            });
        taskSize_++;

        // ��Ϊ����������������п϶������ˣ�notEmpty_֪ͨ,�Ͽ�����߳�ִ������
        notEmpty_.notify_all();

        // cacheģʽ ������ȽϽ��� ������С��������� ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳���
        if (poolMood_ == PoolMood::MOOD_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
        {
            std::cout << "create new thread... " << std::endl;
            // �������̶߳���
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // threads_.emplace_back(std::move(ptr));
            threads_[threadId]->start();
            // �޸��̸߳�����صı���
            curThreadSize_++;
            idleThreadSize_++;
        }

        // ���������Result����
        return result;
    }

    // �����̳߳�
    void start(int initThreadSize = std::thread::hardware_concurrency())
    {
        // �����̳߳ص�����״̬
        isPoolRunning_ = true;
        // ��¼��ʼ���̸߳���
        initThreadSize_ = initThreadSize;
        curThreadSize_ = initThreadSize;

        // �����̶߳���
        for (int i = 0; i < initThreadSize_; i++)
        {
            // ����thread�̶߳����ʱ�򣬰��̺߳�������thread�̶߳���
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int threadId = ptr->getId();
            threads_.emplace(threadId, std::move(ptr));
            // threads_.emplace_back(std::move(ptr));
        }
        // ���������߳� std::vector<Thread *> threads_;
        for (int i = 0; i < initThreadSize_; i++)
        {
            threads_[i]->start(); // ��Ҫȥִ��һ���̺߳���
            idleThreadSize_++;    // ��¼��ʼ����  �̵߳�����
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // �����̺߳���
    void threadFunc(int threadid)
    {
        auto lastTime = std::chrono::high_resolution_clock().now();
        for (;;)
        {
            Task task;
            {
                // ��ȡ��
                std::unique_lock<std::mutex> lock(taskQueMtx_);
                std::cout << "tid" << std::this_thread::get_id() << " ���Ի�ȡ����... " << std::endl;
                // cacheģʽ�£��п��ܴ����˺ܶ���̣߳����ǿ���ʱ�䳬��60s��Ӧ�ðѶ����̻߳��յ� ��Ҫ֪����ǰʱ�����һ��ִ�е�ʱ�� > 60s

                // ÿһ���ӷ���һ�� ��ô���֣���ʱ���أ������������ִ�з���
                // ��+˫���ж�
                while (taskQue_.size() == 0)
                {
                    // �̳߳�Ҫ���� �����߳���Դ
                    if (!isPoolRunning_)
                    {
                        threads_.erase(threadid);
                        std::cout << "threadid: " << std::this_thread::get_id() << "exit" << std::endl;
                        exitCond_.notify_all();
                        return; // �̺߳������� �߳̽���
                    }
                    if (poolMood_ == PoolMood::MOOD_CACHED)
                    {
                        // ����������ʱ������
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
                        // �ȴ�notEmpty����
                        notEmpty_.wait(lock); // �����Ǽ�������ִ��
                    }
                }

                idleThreadSize_--;
                std::cout << "tid" << std::this_thread::get_id() << " ��ȡ����ɹ�... " << std::endl;
                // �����������ȡһ���������
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;
                // �����Ȼ��ʣ�����񣬼���֪ͨ�������߳�ִ������
                if (taskQue_.size() > 0)
                {
                    notEmpty_.notify_all();
                }
                // ȡ��һ���������֪ͨ��ͨ�����Լ����ύ��������
                notFull_.notify_all();
            } // ��Ӧ�ð����ͷŵ�
            // ��ǰ�̸߳���ִ���������
            if (task != nullptr)
            {
                task();  // ִ��function<void()>
            }
            idleThreadSize_++;
            lastTime = std::chrono::high_resolution_clock().now(); // �����߳�ִ���������ʱ��
        }
    }

    // ���pool������״̬
    bool checkRunningState() const
    {
        return isPoolRunning_;
    }

private:
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // �߳��б�
    size_t initThreadSize_;                                    // ��ʼ���߳�����
    std::atomic_int curThreadSize_;                            // ��¼��ǰ�̳߳�������̵߳�������
    int threadSizeThreshHold_;                                 // �߳�������������ֵ
    std::atomic_int idleThreadSize_;                           // ��¼�����̵߳�����

    // Task���� =����������
    using Task = std::function<void()>;
    std::queue<Task> taskQue_; // �������
    std::atomic_int taskSize_; // ���������
    int taskQueMaxThreadHold_; // ��������ϵ���ֵ

    std::mutex taskQueMtx_;            // ��֤������е��̰߳�ȫ
    std::condition_variable notFull_;  // ��ʾ������в���
    std::condition_variable notEmpty_; // ��ʾ������в���
    std::condition_variable exitCond_; // �ȴ��߳���Դ����

    PoolMood poolMood_;              // ��ǰ�̳߳صĹ���ģʽ
    std::atomic_bool isPoolRunning_; // ��ʾ��ǰ�̳߳ص�����״̬
};

#endif