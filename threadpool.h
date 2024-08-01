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

// Any���ͣ����Խ��������������� ģ����Ĵ���ֻ��д��ͷ�ļ���
class Any
{
public:
    Any() = default; // Ĭ�Ϲ��캯��
    ~Any() = default; // Ĭ����������
    Any(const Any&) = delete; // ��ֹ��������
    Any& operator=(const Any&) = delete; // ��ֹ������ֵ
    Any(Any&&) = default; // �ƶ����캯��������Any��������ƶ�
    Any& operator=(Any&&) = default; // �ƶ���ֵ�����
    // ���캯���������������͵�data�����洢�ڻ���ָ��base_��
    template <typename T>
    Any(T data) : base_(std::make_unique<Derive<T>>(data)) {}
    // �ṩһ���������洢��data��ȡ����
    template <typename T>
    T cast_()
    {
        // ������ָ��ת��Ϊ������ָ�룬����ȡ�洢�ľ������͵�����
        Derive<T>* pd = dynamic_cast<Derive<T> *>(base_.get());
        if (pd == nullptr)
        {
            throw "type is unmatch"; // ������Ͳ�ƥ�䣬���׳��쳣
        }
        return pd->data_;
    }
private:
    // ����һ���������ͣ����ڴ洢�������͵�����
    class Base
    {
    public:
        virtual ~Base() = default; // ����������
    };
    // ����һ��������ģ�壬�洢�������������
    template <typename T>
    class Derive : public Base
    {
    public:
        Derive(T data) : data_(data) {} // ���캯������ʼ��data_
        T data_; // �洢ʵ�ʵ�����
    };
private:
    // ����ָ�룬����ָ�����������ʵ�����͵ĳ���
    std::unique_ptr<Base> base_;
};

// ʵ��һ���ź�����
class Semaphore
{
public:
    Semaphore(int limit = 0) : resLimit_(0), isExit_(false) {}
    ~Semaphore()
    {
        isExit_ = true;
    }
    // ��ȡһ���ź�����Դ
    void wait()
    {
        if (isExit_)
            return;
        std::unique_lock<std::mutex> lock(mtx_);
        // �ȴ��ź�������Դ,û����Դ�Ļ�����������ǰ�߳�
        cond_.wait(lock, [&]() -> bool
            { return resLimit_ > 0; });
        resLimit_--;
    }
    // ����һ���ź�����Դ
    void post()
    {
        if (isExit_)
            return;
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        // linux��cond����������ʲôҲû�� ��������״̬�Ѿ�ʧЧ���޹�����
        cond_.notify_all();
    }

private:
    std::atomic_bool isExit_;
    int resLimit_;
    std::mutex mtx_;
    std::condition_variable cond_;
};

// ���͵�ǰ������
class Task;
// ʵ�ֽ����ύ���̳߳�task����ִ�к�ķ���ֵ����Result
class Result
{
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    // setVal��������ȡ����ִ����Ϸ���ֵ��
    void setVal(Any any);
    // get�������û��������������ȡtask�ķ���ֵ
    Any get();

private:
    Any any_;                    // �洢����ķ���ֵ
    Semaphore sem_;              // �߳�ͨ���ź���
    std::shared_ptr<Task> task_; // ָ���Ӧ��ȡ����ֵ���������
    std::atomic_bool isValid_;   // ����ֵ�Ƿ���Ч
};

// ����������
class Task
{
public:
    Task();
    ~Task() = default;
    void exec();
    void setResult(Result* res);
    // �û������Զ��������������񣬴�task�̳У���дrun������ʵ���Զ���������
    virtual Any run() = 0;

private:
    Result* result_; // Result���������������Ҫ���� Task ��
};

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
    Thread(ThreadFunc func);
    // �߳�����
    ~Thread();
    // �����߳�
    void start();
    // ��ȡ�߳�id
    int getId() const;

private:
    ThreadFunc func_;
    static int generateId_;
    int threadId_; // �����߳�id
};
// �̳߳�����
class ThreadPool
{
public:
    // �̳߳ع���
    ThreadPool();

    // �̳߳�����
    ~ThreadPool();

    // �����̳߳صĹ���ģʽ
    void setMood(PoolMood mode);

    // ����task����������޵���ֵ
    void setTaskQueMaxThreadHold(int threadhold);

    // �����̳߳�cachedģʽ�µ��߳���ֵ
    void setThreadSizeThreshHold(int threadhold);

    // ���̳߳��ύ����
    Result submitTask(std::shared_ptr<Task> sp);

    // �����̳߳�
    void start(int initThreadSize = std::thread::hardware_concurrency());

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    // �����̺߳���
    void threadFunc(int threadid);
    // ���pool������״̬
    bool checkRunningState() const;

private:
    // std::vector<std::unique_ptr<Thread>> threads_; // �߳��б�
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // �߳��б�
    size_t initThreadSize_;                                    // ��ʼ���߳�����
    std::atomic_int curThreadSize_;                            // ��¼��ǰ�̳߳�������̵߳�������
    int threadSizeThreshHold_;                                 // �߳�������������ֵ
    std::atomic_int idleThreadSize_;                           // ��¼�����̵߳�����

    std::queue<std::shared_ptr<Task>> taskQue_; // �������
    std::atomic_int taskSize_;                  // ���������
    int taskQueMaxThreadHold_;                  // ��������ϵ���ֵ

    std::mutex taskQueMtx_;            // ��֤������е��̰߳�ȫ
    std::condition_variable notFull_;  // ��ʾ������в���
    std::condition_variable notEmpty_; // ��ʾ������в���
    std::condition_variable exitCond_; // �ȴ��߳���Դ����

    PoolMood poolMood_;              // ��ǰ�̳߳صĹ���ģʽ
    std::atomic_bool isPoolRunning_; // ��ʾ��ǰ�̳߳ص�����״̬
};

#endif