// Copyright 2014 GitHub, Inc.
// Copyright 2017 Cheng Zhao. All rights reserved.
// Use of this source code is governed by the MIT license.

#include "qode/integration/node_integration.h"

#include <string>
#include <vector>

#include "uv/src/uv-common.h"
#include "env-inl.h"
#include "node.h"
#include "node_internals.h"

namespace qode {

NodeIntegration::NodeIntegration()
    : uv_loop_(uv_default_loop()),
      embed_closed_(false) {
}

constexpr uint64_t EVENT_BATCH_TIMEOUT_MS = 8;
constexpr int EVENT_BATCH_SIZE = 16;

NodeIntegration::~NodeIntegration() {
  // Quit the embed thread.
  embed_closed_ = true;
  uv_sem_post(&embed_sem_);
  WakeupEmbedThread();

  // Wait for everything to be done.
  uv_thread_join(&embed_thread_);

  // Clear uv.
  uv_sem_destroy(&embed_sem_);
  uv_close(reinterpret_cast<uv_handle_t*>(&wakeup_handle_), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&next_tick_handle_), nullptr);
}

void NodeIntegration::Init() {
  // Handle used for waking up the uv loop in embed thread.
  // Note that we does not unref this handle to keep the active_handles > 0.
  uv_async_init(uv_loop_, &wakeup_handle_, nullptr);

  // Handle used for invoking CallNextTick.
  uv_async_init(uv_loop_, &next_tick_handle_, &OnCallNextTick);
  uv_unref(reinterpret_cast<uv_handle_t*>(&next_tick_handle_));

  // Start worker that will interrupt main loop when having uv events.
  uv_sem_init(&embed_sem_, 0);
  uv_thread_create(&embed_thread_, EmbedThreadRunner, this);
}

void NodeIntegration::UvRunOnce() {
  // Get current env.
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  node::Environment* env = node::Environment::GetCurrent(isolate);
  CHECK(env);

  // Enter node context while dealing with uv events.
  v8::Context::Scope context_scope(env->context());

  // Perform microtask checkpoint after running JavaScript.
  v8::MicrotasksScope::PerformCheckpoint(isolate);

  // Deal with uv events.
  uv_run(uv_loop_, UV_RUN_NOWAIT);
}

void NodeIntegration::CallNextTick() {
  uv_async_send(&next_tick_handle_);
}

void NodeIntegration::ReleaseHandleRef() {
  uv_unref(reinterpret_cast<uv_handle_t*>(&wakeup_handle_));
}

void NodeIntegration::WakeupMainThread() {
  PostTask([this] {
    this->UvRunOnce();
    // ^ This also updates the uv_now() timestamp.
    uint64_t start_time_ms = uv_now(uv_loop_);

    int loop_count = EVENT_BATCH_SIZE;
    uint64_t elapsed_ms = 0;
    while (loop_count != 0 && (elapsed_ms < EVENT_BATCH_TIMEOUT_MS)) {
      loop_count--;
      this->UvRunOnce();
      elapsed_ms = uv_now(uv_loop_) - start_time_ms;
    }

    // Tell the worker thread to continue polling.
    uv_sem_post(&this->embed_sem_);
  });
}

void NodeIntegration::WakeupEmbedThread() {
  uv_async_send(&wakeup_handle_);
}

// static
void NodeIntegration::EmbedThreadRunner(void *arg) {
  NodeIntegration* self = static_cast<NodeIntegration*>(arg);

  while (true) {
    if (self->embed_closed_)
      break;

    // Wait for something to happen in uv loop.
    // Note that the PollEvents() is implemented by derived classes, so when
    // this class is being destructed the PollEvents() would not be available
    // anymore. Because of it we must make sure we only invoke PollEvents()
    // when this class is alive.
    self->PollEvents();
    if (self->embed_closed_)
      break;

    // Break the loop when uv has decided to quit.
    if (self->uv_loop_->stop_flag != 0 ||
        (!uv__has_active_handles(self->uv_loop_) &&
         !uv__has_active_reqs(self->uv_loop_)))
      break;

    // Deal with event in main thread.
    self->WakeupMainThread();

    // Wait for the main loop to deal with events.
    uv_sem_wait(&self->embed_sem_);
  }
}

// static
void NodeIntegration::OnCallNextTick(uv_async_t* handle) {
  // Get current env.
  v8::Isolate* isolate = v8::Isolate::GetCurrent();
  v8::HandleScope handle_scope(isolate);
  node::Environment* env = node::Environment::GetCurrent(isolate);
  CHECK(env);

  // The CallbackScope can handle everything for us.
  v8::Context::Scope context_scope(env->context());
  node::CallbackScope scope(env->isolate(), v8::Object::New(env->isolate()),
                            {0, 0});
}

}  // namespace qode