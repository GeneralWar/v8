// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler-dispatcher/compiler-dispatcher.h"

#include "src/ast/ast.h"
#include "src/base/platform/time.h"
#include "src/base/template-utils.h"
#include "src/cancelable-task.h"
#include "src/compiler.h"
#include "src/flags.h"
#include "src/global-handles.h"
#include "src/objects-inl.h"
#include "src/parsing/parse-info.h"
#include "src/parsing/parser.h"

namespace v8 {
namespace internal {

CompilerDispatcher::Job::Job(BackgroundCompileTask* task_arg)
    : task(task_arg), has_run(false) {}

CompilerDispatcher::Job::~Job() = default;

CompilerDispatcher::CompilerDispatcher(Isolate* isolate, Platform* platform,
                                       size_t max_stack_size)
    : isolate_(isolate),
      allocator_(isolate->allocator()),
      worker_thread_runtime_call_stats_(
          isolate->counters()->worker_thread_runtime_call_stats()),
      background_compile_timer_(
          isolate->counters()->compile_function_on_background()),
      taskrunner_(platform->GetForegroundTaskRunner(
          reinterpret_cast<v8::Isolate*>(isolate))),
      platform_(platform),
      max_stack_size_(max_stack_size),
      trace_compiler_dispatcher_(FLAG_trace_compiler_dispatcher),
      task_manager_(new CancelableTaskManager()),
      next_job_id_(0),
      shared_to_unoptimized_job_id_(isolate->heap()),
      memory_pressure_level_(MemoryPressureLevel::kNone),
      abort_(false),
      idle_task_scheduled_(false),
      num_worker_tasks_(0),
      main_thread_blocking_on_job_(nullptr),
      block_for_testing_(false),
      semaphore_for_testing_(0) {
  if (trace_compiler_dispatcher_ && !IsEnabled()) {
    PrintF("CompilerDispatcher: dispatcher is disabled\n");
  }
}

CompilerDispatcher::~CompilerDispatcher() {
  // To avoid crashing in unit tests due to unfished jobs.
  AbortAll(BlockingBehavior::kBlock);
  task_manager_->CancelAndWait();
}

bool CompilerDispatcher::CanEnqueue() {
  if (!IsEnabled()) return false;

  // TODO(rmcilroy): Investigate if MemoryPressureLevel::kNone is ever sent on
  // Android, if not, remove this check.
  if (memory_pressure_level_.Value() != MemoryPressureLevel::kNone) {
    return false;
  }

  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    if (abort_) return false;
  }

  return true;
}

base::Optional<CompilerDispatcher::JobId> CompilerDispatcher::Enqueue(
    const ParseInfo* outer_parse_info, const AstRawString* function_name,
    const FunctionLiteral* function_literal) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.CompilerDispatcherEnqueue");
  RuntimeCallTimerScope runtimeTimer(
      isolate_, RuntimeCallCounterId::kCompileEnqueueOnDispatcher);

  if (!CanEnqueue()) return base::nullopt;

  std::unique_ptr<Job> job = base::make_unique<Job>(new BackgroundCompileTask(
      allocator_, outer_parse_info, function_name, function_literal,
      worker_thread_runtime_call_stats_, background_compile_timer_,
      static_cast<int>(max_stack_size_)));
  JobMap::const_iterator it = InsertJob(std::move(job));
  JobId id = it->first;
  if (trace_compiler_dispatcher_) {
    PrintF("CompilerDispatcher: enqueued job %zu for function literal id %d\n",
           id, function_literal->function_literal_id());
  }

  // Post a a background worker task to perform the compilation on the worker
  // thread.
  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    pending_background_jobs_.insert(it->second.get());
  }
  ScheduleMoreWorkerTasksIfNeeded();
  return base::make_optional(id);
}

bool CompilerDispatcher::IsEnabled() const { return FLAG_compiler_dispatcher; }

bool CompilerDispatcher::IsEnqueued(Handle<SharedFunctionInfo> function) const {
  if (jobs_.empty()) return false;
  return GetJobFor(function) != jobs_.end();
}

bool CompilerDispatcher::IsEnqueued(JobId job_id) const {
  return jobs_.find(job_id) != jobs_.end();
}

void CompilerDispatcher::RegisterSharedFunctionInfo(
    JobId job_id, SharedFunctionInfo* function) {
  DCHECK_NE(jobs_.find(job_id), jobs_.end());

  if (trace_compiler_dispatcher_) {
    PrintF("CompilerDispatcher: registering ");
    function->ShortPrint();
    PrintF(" with job id %zu\n", job_id);
  }

  // Make a global handle to the function.
  Handle<SharedFunctionInfo> function_handle =
      isolate_->global_handles()->Create(function);

  // Register mapping.
  auto job_it = jobs_.find(job_id);
  DCHECK_NE(job_it, jobs_.end());
  Job* job = job_it->second.get();
  shared_to_unoptimized_job_id_.Set(function_handle, job_id);

  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    job->function = function_handle;
    if (job->IsReadyToFinalize(lock)) {
      // Schedule an idle task to finalize job if it is ready.
      ScheduleIdleTaskFromAnyThread(lock);
    }
  }
}

void CompilerDispatcher::WaitForJobIfRunningOnBackground(Job* job) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.CompilerDispatcherWaitForBackgroundJob");
  RuntimeCallTimerScope runtimeTimer(
      isolate_, RuntimeCallCounterId::kCompileWaitForDispatcher);

  base::LockGuard<base::Mutex> lock(&mutex_);
  if (running_background_jobs_.find(job) == running_background_jobs_.end()) {
    pending_background_jobs_.erase(job);
    return;
  }
  DCHECK_NULL(main_thread_blocking_on_job_);
  main_thread_blocking_on_job_ = job;
  while (main_thread_blocking_on_job_ != nullptr) {
    main_thread_blocking_signal_.Wait(&mutex_);
  }
  DCHECK(pending_background_jobs_.find(job) == pending_background_jobs_.end());
  DCHECK(running_background_jobs_.find(job) == running_background_jobs_.end());
}

bool CompilerDispatcher::FinishNow(Handle<SharedFunctionInfo> function) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.CompilerDispatcherFinishNow");
  RuntimeCallTimerScope runtimeTimer(
      isolate_, RuntimeCallCounterId::kCompileFinishNowOnDispatcher);
  if (trace_compiler_dispatcher_) {
    PrintF("CompilerDispatcher: finishing ");
    function->ShortPrint();
    PrintF(" now\n");
  }

  JobMap::const_iterator it = GetJobFor(function);
  CHECK(it != jobs_.end());
  Job* job = it->second.get();
  WaitForJobIfRunningOnBackground(job);

  if (!job->has_run) {
    job->task->Run();
    job->has_run = true;
  }

  DCHECK(job->IsReadyToFinalize(&mutex_));
  bool success = Compiler::FinalizeBackgroundCompileTask(
      job->task.get(), function, isolate_, Compiler::KEEP_EXCEPTION);

  DCHECK_NE(success, isolate_->has_pending_exception());
  RemoveJob(it);
  return success;
}

void CompilerDispatcher::AbortAll(BlockingBehavior blocking) {
  bool background_tasks_running =
      task_manager_->TryAbortAll() == CancelableTaskManager::kTaskRunning;
  if (!background_tasks_running || blocking == BlockingBehavior::kBlock) {
    for (auto& it : jobs_) {
      WaitForJobIfRunningOnBackground(it.second.get());
      if (trace_compiler_dispatcher_) {
        PrintF("CompilerDispatcher: aborted job %zu\n", it.first);
      }
    }
    jobs_.clear();
    shared_to_unoptimized_job_id_.Clear();
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      DCHECK(pending_background_jobs_.empty());
      DCHECK(running_background_jobs_.empty());
      abort_ = false;
    }
    return;
  }

  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    abort_ = true;
    pending_background_jobs_.clear();
    idle_task_scheduled_ = false;  // Idle task cancelled by TryAbortAll.
  }
  AbortInactiveJobs();

  // All running background jobs might already have scheduled idle tasks instead
  // of abort tasks. Schedule a single abort task here to make sure they get
  // processed as soon as possible (and not first when we have idle time).
  ScheduleAbortTask();
}

void CompilerDispatcher::AbortInactiveJobs() {
  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    // Since we schedule two abort tasks per async abort, we might end up
    // here with nothing left to do.
    if (!abort_) return;
  }
  for (auto it = jobs_.cbegin(); it != jobs_.cend();) {
    auto job = it;
    ++it;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      if (running_background_jobs_.find(job->second.get()) !=
          running_background_jobs_.end()) {
        continue;
      }
    }
    if (trace_compiler_dispatcher_) {
      PrintF("CompilerDispatcher: aborted job %zu\n", job->first);
    }
    it = RemoveJob(job);
  }
  if (jobs_.empty()) {
    base::LockGuard<base::Mutex> lock(&mutex_);
    if (num_worker_tasks_ == 0) abort_ = false;
  }
}

void CompilerDispatcher::MemoryPressureNotification(
    v8::MemoryPressureLevel level, bool is_isolate_locked) {
  MemoryPressureLevel previous = memory_pressure_level_.Value();
  memory_pressure_level_.SetValue(level);
  // If we're already under pressure, we haven't accepted new tasks meanwhile
  // and can just return. If we're no longer under pressure, we're also done.
  if (previous != MemoryPressureLevel::kNone ||
      level == MemoryPressureLevel::kNone) {
    return;
  }
  if (trace_compiler_dispatcher_) {
    PrintF("CompilerDispatcher: received memory pressure notification\n");
  }
  if (is_isolate_locked) {
    AbortAll(BlockingBehavior::kDontBlock);
  } else {
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      if (abort_) return;
      // By going into abort mode here, and clearing the
      // pending_background_jobs_, we at keep existing background jobs from
      // picking up more work before the MemoryPressureTask gets executed.
      abort_ = true;
      pending_background_jobs_.clear();
    }
    taskrunner_->PostTask(MakeCancelableLambdaTask(task_manager_.get(), [this] {
      AbortAll(BlockingBehavior::kDontBlock);
    }));
  }
}

CompilerDispatcher::JobMap::const_iterator CompilerDispatcher::GetJobFor(
    Handle<SharedFunctionInfo> shared) const {
  JobId* job_id_ptr = shared_to_unoptimized_job_id_.Find(shared);
  JobMap::const_iterator job = jobs_.end();
  if (job_id_ptr) {
    job = jobs_.find(*job_id_ptr);
  }
  return job;
}

void CompilerDispatcher::ScheduleIdleTaskFromAnyThread(
    const base::LockGuard<base::Mutex>&) {
  if (!taskrunner_->IdleTasksEnabled()) return;
  if (idle_task_scheduled_ || abort_) return;

  idle_task_scheduled_ = true;
  taskrunner_->PostIdleTask(MakeCancelableIdleLambdaTask(
      task_manager_.get(),
      [this](double deadline_in_seconds) { DoIdleWork(deadline_in_seconds); }));
}

void CompilerDispatcher::ScheduleAbortTask() {
  taskrunner_->PostTask(MakeCancelableLambdaTask(
      task_manager_.get(), [this] { AbortInactiveJobs(); }));
}

void CompilerDispatcher::ScheduleMoreWorkerTasksIfNeeded() {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.CompilerDispatcherScheduleMoreWorkerTasksIfNeeded");
  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    if (pending_background_jobs_.empty()) return;
    if (platform_->NumberOfWorkerThreads() <= num_worker_tasks_) {
      return;
    }
    ++num_worker_tasks_;
  }
  platform_->CallOnWorkerThread(MakeCancelableLambdaTask(
      task_manager_.get(), [this] { DoBackgroundWork(); }));
}

void CompilerDispatcher::DoBackgroundWork() {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.CompilerDispatcherDoBackgroundWork");
  for (;;) {
    Job* job = nullptr;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      if (!pending_background_jobs_.empty()) {
        auto it = pending_background_jobs_.begin();
        job = *it;
        pending_background_jobs_.erase(it);
        running_background_jobs_.insert(job);
      }
    }
    if (job == nullptr) break;

    if (V8_UNLIKELY(block_for_testing_.Value())) {
      block_for_testing_.SetValue(false);
      semaphore_for_testing_.Wait();
    }

    if (trace_compiler_dispatcher_) {
      PrintF("CompilerDispatcher: doing background work\n");
    }

    job->task->Run();

    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      running_background_jobs_.erase(job);

      job->has_run = true;
      if (job->IsReadyToFinalize(lock)) {
        // Schedule an idle task to finalize the compilation on the main thread
        // if the job has a shared function info registered.
        ScheduleIdleTaskFromAnyThread(lock);
      }

      if (main_thread_blocking_on_job_ == job) {
        main_thread_blocking_on_job_ = nullptr;
        main_thread_blocking_signal_.NotifyOne();
      }
    }
  }

  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    --num_worker_tasks_;

    if (running_background_jobs_.empty() && abort_) {
      // This is the last background job that finished. The abort task
      // scheduled by AbortAll might already have ran, so schedule another
      // one to be on the safe side.
      ScheduleAbortTask();
    }
  }
  // Don't touch |this| anymore after this point, as it might have been
  // deleted.
}

void CompilerDispatcher::DoIdleWork(double deadline_in_seconds) {
  TRACE_EVENT0(TRACE_DISABLED_BY_DEFAULT("v8.compile"),
               "V8.CompilerDispatcherDoIdleWork");
  bool aborted = false;
  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    idle_task_scheduled_ = false;
    aborted = abort_;
  }

  if (aborted) {
    AbortInactiveJobs();
    return;
  }

  if (trace_compiler_dispatcher_) {
    PrintF("CompilerDispatcher: received %0.1lfms of idle time\n",
           (deadline_in_seconds - platform_->MonotonicallyIncreasingTime()) *
               static_cast<double>(base::Time::kMillisecondsPerSecond));
  }
  while (deadline_in_seconds > platform_->MonotonicallyIncreasingTime()) {
    // Find a job which is pending finalization and has a shared function info
    CompilerDispatcher::JobMap::const_iterator it;
    {
      base::LockGuard<base::Mutex> lock(&mutex_);
      for (it = jobs_.cbegin(); it != jobs_.cend(); ++it) {
        if (it->second->IsReadyToFinalize(lock)) break;
      }
      // Since we hold the lock here, we can be sure no jobs have become ready
      // for finalization while we looped through the list.
      if (it == jobs_.cend()) return;

      DCHECK(it->second->IsReadyToFinalize(lock));
      DCHECK_EQ(running_background_jobs_.find(it->second.get()),
                running_background_jobs_.end());
      DCHECK_EQ(pending_background_jobs_.find(it->second.get()),
                pending_background_jobs_.end());
    }

    Job* job = it->second.get();
    Compiler::FinalizeBackgroundCompileTask(
        job->task.get(), job->function.ToHandleChecked(), isolate_,
        Compiler::CLEAR_EXCEPTION);
    RemoveJob(it);
  }

  // We didn't return above so there still might be jobs to finalize.
  {
    base::LockGuard<base::Mutex> lock(&mutex_);
    ScheduleIdleTaskFromAnyThread(lock);
  }
}

CompilerDispatcher::JobMap::const_iterator CompilerDispatcher::InsertJob(
    std::unique_ptr<Job> job) {
  bool added;
  JobMap::const_iterator it;
  std::tie(it, added) =
      jobs_.insert(std::make_pair(next_job_id_++, std::move(job)));
  DCHECK(added);
  return it;
}

CompilerDispatcher::JobMap::const_iterator CompilerDispatcher::RemoveJob(
    CompilerDispatcher::JobMap::const_iterator it) {
  Job* job = it->second.get();

  DCHECK_EQ(running_background_jobs_.find(job), running_background_jobs_.end());
  DCHECK_EQ(pending_background_jobs_.find(job), pending_background_jobs_.end());

  // Delete SFI associated with job if its been registered.
  Handle<SharedFunctionInfo> function;
  if (job->function.ToHandle(&function)) {
    GlobalHandles::Destroy(Handle<Object>::cast(function).location());
  }

  // Delete job.
  it = jobs_.erase(it);
  if (jobs_.empty()) {
    base::LockGuard<base::Mutex> lock(&mutex_);
    if (num_worker_tasks_ == 0) abort_ = false;
  }

  return it;
}

}  // namespace internal
}  // namespace v8
