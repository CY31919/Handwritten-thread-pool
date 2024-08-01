#include "threadpool.h"
#include <functional>
#include <thread>
#include <iostream>

const int TASK_MAX_THREADHOLD = 1024;
const int THREAD_MAX_THRESHHOLD = 100;
const int THREAD_MAX_IDLE_TIME = 0.00001; // ��λ����

// �̳߳ع���
ThreadPool::ThreadPool() :
    initThreadSize_(0)
    , taskSize_(0)
    , threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
    , curThreadSize_(0)
    ,taskQueMaxThreadHold_(TASK_MAX_THREADHOLD)
    , poolMood_(PoolMood::MOOD_FIXED)
    , isPoolRunning_(false)
    , idleThreadSize_(0){}

// �̳߳�����
ThreadPool::~ThreadPool()
{
    isPoolRunning_ = false;
    // �ȴ��̳߳����е��̷߳��أ�������״̬������ & ����ִ��������
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    notEmpty_.notify_all();
    exitCond_.wait(lock, [&]() -> bool{ return threads_.size() == 0; });
}

// �����̳߳صĹ���ģʽ
void ThreadPool::setMood(PoolMood mode)
{
    if (checkRunningState())
        return;
    poolMood_ = mode;
}

// ����task����������޵���ֵ
void ThreadPool::setTaskQueMaxThreadHold(int threadhold)
{
    taskQueMaxThreadHold_ = threadhold;
}

// �����̳߳�cachedģʽ�µ��߳���ֵ
void ThreadPool::setThreadSizeThreshHold(int threadhold)
{
    if (checkRunningState())
        return;

    if (poolMood_ == PoolMood::MOOD_CACHED)
    {
        threadSizeThreshHold_ = threadhold;
    }
}

// ���̳߳��ύ���� �û����øýӿ� ������� ��������
Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    //�����ѻ��ߵȴ�һ���������
    if (!notFull_.wait_for(lock, std::chrono::seconds(1),
        [&]() -> bool
        { return taskQue_.size() < (size_t)taskQueMaxThreadHold_; })) 
    {
        std::cerr << "task queue is full,submit task fail" << std::endl;
        return Result(sp, false);
    }
    taskQue_.emplace(sp);
    taskSize_++;
    notEmpty_.notify_all();
    // cacheģʽ  ������С��������񣬲���ռ��̫��ʱ���߳� 
    // ��Ҫ�������������Ϳ����̵߳��������ж��Ƿ���Ҫ�����µ��̳߳���
    if (poolMood_ == PoolMood::MOOD_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeThreshHold_)
    {
        std::cout << "create new thread... " << std::endl;
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        std::cout << "��Ӵ���" << std::endl;
        threads_[threadId]->start();
        curThreadSize_++;
        idleThreadSize_++;
    }
  
    return Result(sp);
}

// �����̳߳�
void ThreadPool::start(int initThreadSize)
{
    // �����̳߳ص�����״̬
    isPoolRunning_ = true;
    // ��¼��ʼ���̸߳���
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;

    // �����̶߳���
    for (int i = 0; i < initThreadSize_; i++)
    {
        // ����ָ��thread�̶߳��������ָ�룬�ڴ��������а��̺߳�������thread�̶߳���
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getId();
        threads_.emplace(threadId, std::move(ptr));
        std::cout << "��ʼ����" << std::endl;
        // threads_.emplace_back(std::move(ptr));
    }
    // ���������߳� std::vector<Thread *> threads_;
    for (int i = 0; i < initThreadSize_; i++)
    {
        threads_[i]->start(); // ��Ҫȥִ��һ���̺߳���
        idleThreadSize_++;    // ��¼��ʼ����  �̵߳�����
    }
}

// �����̺߳��� �̳߳ص������̴߳���������������߳�
// �̺߳������أ���Ӧ���߳�Ҳ�ͽ�����
void ThreadPool::threadFunc(int threadid) 
{
    auto lastTime = std::chrono::high_resolution_clock().now();
   for(;;)
    {
        std::shared_ptr<Task> task;
        {
            std::unique_lock<std::mutex> lock(taskQueMtx_);
            std::cout << "tid" << std::this_thread::get_id() << " ���Ի�ȡ����... " << std::endl;
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
                    return;  // �̺߳������� �߳̽���
                }
                if (poolMood_ == PoolMood::MOOD_CACHED)
                {
                    //1s��û����������ӽ����������Ϊcv_status::timeout
                    if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)))
                    {
                        auto nowTime = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(nowTime - lastTime);
                        if (dur.count() >= THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_)
                        {
                            threads_.erase(threadid);
                            curThreadSize_--;
                            idleThreadSize_--;     
                            std::cout << "threadid: " << std::this_thread::get_id() << "���ࡢ��ʱ����ɾ��" << std::endl;
                            return;
                        }
                    }
                }
                else
                {
                    // �ȴ�notEmpty����
                    notEmpty_.wait(lock); // �����Ǽ�������ִ��
                }
                // �̳߳�Ҫ�����������߳���Դ
                if (!isPoolRunning_)
                 {
                    threads_.erase(threadid);
                    std::cout << "threadid: " << std::this_thread::get_id() << "exit" << std::endl;
                   exitCond_.notify_all();
                    return;
                    
                }
            }

            idleThreadSize_--;
            std::cout << "tid" << std::this_thread::get_id() << " ��ȡ����ɹ�... " << std::endl;
            // �����������ȡһ���������
            task = taskQue_.front();
            taskQue_.pop();
            taskSize_--;
      
            if (taskQue_.size() > 0)
            {
                notEmpty_.notify_all();//֪ͨ���������߿�������
            }
            notFull_.notify_all();//֪ͨ�����߼�������
        }
       
        if (task != nullptr)
        {
            // task->run();  // ִ�����񣺰�����ķ���ֵsetVal��������Result
            task->exec();
        }
        idleThreadSize_++;
        lastTime = std::chrono::high_resolution_clock().now(); // �����߳�ִ���������ʱ��
    }

 }

bool ThreadPool::checkRunningState() const
{
    return isPoolRunning_;
}

//////////////// �̷߳���ʵ��

int Thread::generateId_ = 0;

// �̹߳���
Thread::Thread(ThreadFunc func) : func_(func), threadId_(generateId_++)
{
}
// �߳�����
Thread::~Thread()
{
}

// �����߳�
void Thread::start()
{
    // ����һ���߳���ִ��һ���̺߳���
    std::thread t(func_, threadId_); // C++��˵ �̶߳���t �� �̺߳��� func_
    t.detach();                      // ���÷����߳�
}

// ��ȡ�߳�id
int Thread::getId() const
{
    return threadId_;
}

///////////////////////Task����ʵ��
Task::Task() : result_(nullptr)
{
}
void Task::exec() // �൱�ڶ�����һ���װ
{
    if (result_ != nullptr)
    {
        result_->setVal(run()); // ����ʵ�ֵĶ�̬����
    }
}
void Task::setResult(Result* res)
{
    result_ = res;
}


// Reslut ������ʵ��
Result::Result(std::shared_ptr<Task> task, bool isValid)
    : task_(task), isValid_(isValid)
{
    task_->setResult(this);
}
// Result::~Result(){}
Any Result::get() // �û����õ�
{
    if (!isValid_)
    {
        return "";
    }
    sem_.wait(); // �������û��ִ���꣬����������û����߳�
    return std::move(any_);
}
void Result::setVal(Any any) 
{
    // �洢task�ķ���ֵ
    this->any_ = std::move(any);
    sem_.post(); // �Ѿ���ȡ������ķ���ֵ����ȡ�ź�����Դ
}