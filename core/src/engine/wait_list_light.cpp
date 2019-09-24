#include "wait_list_light.hpp"

#include <engine/sleep.hpp>
#include <engine/task/task_context.hpp>
#include <utils/assert.hpp>
#include <utils/scope_guard.hpp>

namespace engine {
namespace impl {

WaitListLight::Lock::operator bool() {
  UASSERT_MSG(false, "must not be called");
  return true;
}

WaitListLight::~WaitListLight() = default;

void WaitListLight::PinToCurrentTask() {
#ifndef NDEBUG
  auto task = engine::current_task::GetCurrentTaskContext();
  UASSERT(task);
  PinToTask(*task);
#endif
}

void WaitListLight::PinToTask([[maybe_unused]] impl::TaskContext& ctx) {
  UASSERT(!owner_ || owner_ == &ctx);
#ifndef NDEBUG
  owner_ = &ctx;
#endif
}

bool WaitListLight::IsEmpty(WaitListBase::Lock&) const { return waiting_; }

void WaitListLight::Append(WaitListBase::Lock&,
                           boost::intrusive_ptr<impl::TaskContext> ctx) {
  LOG_TRACE() << "Appending, use_count=" << ctx->use_count();
  UASSERT(!waiting_);
  UASSERT(!owner_ || owner_ == ctx.get());

  auto ptr = ctx.get();
  intrusive_ptr_add_ref(ptr);
  waiting_ = ptr;
}

void WaitListLight::WakeupOne(WaitListBase::Lock&) {
  LOG_TRACE() << "WakeupOne";
  in_wakeup_ = true;
  utils::ScopeGuard guard([this] { in_wakeup_ = false; });

  auto old = waiting_.exchange(nullptr);
  if (old) {
    LOG_TRACE() << "Waking up! use_count=" << old->use_count();
    old->Wakeup(impl::TaskContext::WakeupSource::kWaitList);
    intrusive_ptr_release(old);
  }
}

void WaitListLight::WakeupAll(WaitListBase::Lock& lock) { WakeupOne(lock); }

void WaitListLight::Remove(  //
    [[maybe_unused]] const boost::intrusive_ptr<impl::TaskContext>& ctx) {
  LOG_TRACE() << "remove (cancel)";
  auto old = waiting_.exchange(nullptr);

  UASSERT(!old || old == ctx.get());

  if (old) {
    intrusive_ptr_release(old);
  } else {
    /*
     * Race with WakeupOne()/WakeupAll() has fired. We have to wait until
     * Wakeup*() in another thread has finished, otherwise Wakeup*() might awake
     * the task too late (e.g. after the next Sleep()) which results in a
     * spurious wakeup (very bad, not all sync primitives check for spurious
     * wakeups, e.g. SleepFor()).
     */

    LOG_TRACE() << "Race with Wakeup*(), spinning";
    while (in_wakeup_) {
      std::this_thread::yield();
    }
  }
}

}  // namespace impl
}  // namespace engine
