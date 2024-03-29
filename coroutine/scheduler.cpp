#include "scheduler.h"
#include <ucontext.h>
#include <assert.h>
#include <sys/epoll.h>
#include <stdio.h>

#define Debug(...) do{ if (g_Scheduler.GetOptions().debug) { printf(__VA_ARGS__); printf("\n"); } }while(0)

Scheduler& Scheduler::getInstance()
{
    static Scheduler obj;
    return obj;
}

Scheduler::Scheduler()
{
    epoll_fd = epoll_create(1024);
    if (epoll_fd == -1) {
        perror("CoroutineScheduler init failed. epoll create error:");
        assert(false);
    }
}

Scheduler::~Scheduler()
{
}

ThreadLocalInfo& Scheduler::GetLocalInfo()
{
    static thread_local ThreadLocalInfo info;
    return info;
}

CoroutineOptions& Scheduler::GetOptions()
{
    static CoroutineOptions options;
    return options;
}

void Scheduler::CreateTask(TaskF const& fn)
{
    Task* tk = new Task(fn, GetOptions().stack_size);
    ++task_count_;
    ++runnale_task_count_;
    AddTask(tk);
}

bool Scheduler::IsCoroutine()
{
    return !!GetLocalInfo().current_task;
}

bool Scheduler::IsEmpty()
{
    return task_count_ == 0;
}

void Scheduler::Yield()
{
    Task* tk = GetLocalInfo().current_task;
    if (!tk) return ;

    Debug("yield task(%llu) state=%d", tk->id_, tk->state_);
    swapcontext(&tk->ctx_, &GetLocalInfo().scheduler);
}

uint32_t Scheduler::Run()
{
    ThreadLocalInfo& info = GetLocalInfo();
    info.current_task = NULL;
    uint32_t do_max_count = runnale_task_count_;
    uint32_t do_count = 0;

    Debug("Run --------------------------");

    // 每次Run执行的协程数量不能多于当前runnable协程数量
    // 以防wait状态的协程得不到执行。
    while (do_count < do_max_count)
    {
        uint32_t cnt = std::max((uint32_t)1, std::min(
                    do_max_count / GetOptions().chunk_count,
                    GetOptions().max_chunk_size));
        Debug("want pop %u tasks.", cnt);
        SList<Task> slist = run_task_.pop(cnt);
        Debug("really pop %u tasks.", cnt);
        if (slist.empty()) break;

        SList<Task>::iterator it = slist.begin();
        while (it != slist.end())
        {
            Task* tk = &*it;
            info.current_task = tk;
            Debug("enter task(%llu)", tk->id_);
            swapcontext(&info.scheduler, &tk->ctx_);
            ++do_count;
            Debug("exit task(%llu) state=%d", tk->id_, tk->state_);
            info.current_task = NULL;

            switch (tk->state_) {
                case TaskState::runnable:
                    ++it;
                    break;

                case TaskState::io_block:
                case TaskState::sync_block:
                    --runnale_task_count_;
                    it = slist.erase(it);
                    wait_task_.push(tk);
                    break;

                case TaskState::done:
                default:
                    --task_count_;
                    --runnale_task_count_;
                    it = slist.erase(it);
                    delete tk;
                    break;
            }
        }
        Debug("push %d task return to runnable list", slist.size());
        run_task_.push(slist);
    }

    static thread_local epoll_event evs[1024];
    int n = epoll_wait(epoll_fd, evs, 1024, 1);
    Debug("do_count=%u, do epoll event, n = %d", do_count, n);
    for (int i = 0; i < n; ++i)
    {
        Task* tk = (Task*)evs[i].data.ptr;
        if (tk->unlink())
            AddTask(tk);
    }

    return do_count;
}

void Scheduler::AddTask(Task* tk)
{
    Debug("Add task(%llu) to runnable list.", tk->id_);
    run_task_.push(tk);
}

