#include "vm.hpp"
#include "console.hpp"

#include "object_utils.hpp"

#include "builtin/array.hpp"
#include "builtin/class.hpp"
#include "builtin/fsevent.hpp"
#include "builtin/string.hpp"
#include "builtin/thread.hpp"

#include "dtrace/dtrace.h"

#include <fcntl.h>
#include <sys/file.h>

#include <stdio.h>

#include <ostream>

// read
#include <unistd.h>
#include <sys/uio.h>
#include <sys/types.h>

namespace rubinius {
  namespace console {
    Object* console_request_trampoline(STATE) {
      state->shared().console()->process_requests(state);
      GCTokenImpl gct;
      state->gc_dependent(gct, 0);
      return cNil;
    }

    Object* console_response_trampoline(STATE) {
      state->shared().console()->process_responses(state);
      GCTokenImpl gct;
      state->gc_dependent(gct, 0);
      return cNil;
    }

    Console::Console(STATE)
      : AuxiliaryThread()
      , shared_(state->shared())
      , request_vm_(NULL)
      , response_vm_(NULL)
      , request_(state)
      , response_(state)
      , console_(state)
      , fsevent_(state)
      , request_fd_(-1)
      , response_fd_(-1)
      , request_exit_(false)
      , response_exit_(false)
      , request_list_(0)
    {
      std::string path(shared_.config.vm_fsapi_path.value + "/console");

      request_path_ = path + "-request";
      response_path_ = path + "-response";

      shared_.auxiliary_threads()->register_thread(this);
    }

    Console::~Console() {
      shared_.auxiliary_threads()->unregister_thread(this);
    }

    void Console::wakeup() {
      request_exit_ = true;
      write(request_fd_, "x", 1);
    }

    void Console::cleanup() {
      if(request_fd_ > 0) {
        close(request_fd_);
        unlink(request_path_.c_str());
        request_fd_ = -1;
      }

      if(response_fd_ > 0) {
        close(response_fd_);
        unlink(response_path_.c_str());
        response_fd_ = -1;
      }

      if(request_list_) {
        for(RequestList::const_iterator i = request_list_->begin();
            i != request_list_->end();
            ++i)
        {
          delete[] *i;
        }

        delete request_list_;
        request_list_ = NULL;
      }
    }

    void Console::initialize(STATE) {
      list_lock_.init();
      response_lock_.init();
      response_cond_.init();
      request_list_ = new RequestList;
    }

    void Console::start(STATE) {
      initialize(state);

      Module* mod = as<Module>(G(rubinius)->get_const(state, "Console"));
      Class* cls = as<Class>(mod->get_const(state, "Server"));
      console_.set(cls->send(state, 0, state->symbol("new")));

      cls->set_const(state, state->symbol("RequestPath"),
          String::create(state, request_path_.c_str()));
      cls->set_const(state, state->symbol("ResponsePath"),
          String::create(state, response_path_.c_str()));

      start_threads(state);
    }

    void Console::run(STATE) {
      if(request_.get()->fork_attached(state)) {
        rubinius::bug("Unable to start console request thread");
      }

      if(response_.get()->fork_attached(state)) {
        rubinius::bug("Unable to start console response thread");
      }
    }

    void Console::start_threads(STATE) {
      SYNC(state);

      if(!request_vm_) {
        request_vm_ = state->shared().new_vm();
        request_exit_ = false;
        request_.set(Thread::create(state, request_vm_, G(thread),
                     console_request_trampoline, true));
      }

      if(!response_vm_) {
        response_vm_ = state->shared().new_vm();
        response_exit_ = false;
        response_.set(Thread::create(state, response_vm_, G(thread),
                     console_response_trampoline, true));
      }

      run(state);
    }

    void Console::stop_threads(STATE) {
      SYNC(state);

      if(request_vm_) {
        wakeup();

        pthread_t os = request_vm_->os_thread();
        request_exit_ = true;

        void* return_value;
        pthread_join(os, &return_value);

        request_vm_ = NULL;
      }

      if(response_vm_) {
        pthread_t os = response_vm_->os_thread();
        response_exit_ = true;

        response_cond_.signal();

        void* return_value;
        pthread_join(os, &return_value);

        response_vm_ = NULL;
      }
    }

    void Console::shutdown(STATE) {
      stop_threads(state);
      cleanup();
    }

    void Console::before_exec(STATE) {
      stop_threads(state);
    }

    void Console::after_exec(STATE) {
      start_threads(state);
    }

    void Console::before_fork(STATE) {
      stop_threads(state);
    }

    void Console::after_fork_parent(STATE) {
      start_threads(state);
    }

    void Console::after_fork_child(STATE) {
      if(request_vm_) {
        VM::discard(state, request_vm_);
        request_vm_ = NULL;
      }

      if(response_vm_) {
        VM::discard(state, response_vm_);
        response_vm_ = NULL;
      }

      cleanup();
      start(state);
    }

    int open_file(std::string path) {
      int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0666);

      if(fd < 0) {
        std::ostringstream msg;
        msg << "failed to open console file: " << path << std::endl;
        msg << strerror(errno) << std::endl;

        rubinius::bug(msg.str().c_str());
      }

      return fd;
    }

    char* Console::read_request(STATE) {
      int status = flock(request_fd_, LOCK_EX);
      if(status < 0) return NULL;

      char* buf[1024];

      lseek(request_fd_, 0, SEEK_SET);
      ssize_t bytes = read(request_fd_, buf, 1024);

      char* req = NULL;
      if(bytes > 0) {
        req = new char[bytes+1];
        memcpy(req, buf, bytes);
        req[bytes] = 0;
      }

      status = lseek(request_fd_, 0, SEEK_SET);
      if(status < 0) {
        std::cerr << "read_request: lseek: " << strerror(errno) << std::endl;
      }
      status = ftruncate(request_fd_, 0);
      if(status < 0) {
        std::cerr << "read_request: ftruncate: " << strerror(errno) << std::endl;
      }

      flock(request_fd_, LOCK_UN);

      return req;
    }

    void Console::process_requests(STATE) {
      GCTokenImpl gct;
      RBX_DTRACE_CONST char* thread_name =
        const_cast<RBX_DTRACE_CONST char*>("rbx.console.request");
      request_vm_->set_name(thread_name);

      RUBINIUS_THREAD_START(const_cast<RBX_DTRACE_CONST char*>(thread_name),
                            state->vm()->thread_id(), 1);

      state->vm()->thread->hard_unlock(state, gct, 0);
      state->gc_independent(gct, 0);

      request_fd_ = open_file(request_path_);

      FSEvent* fsevent = FSEvent::create(state);
      fsevent->watch_file(state, request_fd_, request_path_.c_str());
      fsevent_.set(fsevent);

      while(!request_exit_) {
        Object* status = fsevent_.get()->wait_for_event(state);

        if(request_exit_) break;
        if(status->nil_p()) continue;

        char* request = read_request(state);

        if(request) {
          utilities::thread::Mutex::LockGuard lg(list_lock_);

          request_list_->push_back(request);
          response_cond_.signal();
        }
      }

      RUBINIUS_THREAD_STOP(const_cast<RBX_DTRACE_CONST char*>(thread_name),
                           state->vm()->thread_id(), 1);
    }

    void Console::write_response(STATE, const char* response, native_int size) {
      int status = flock(response_fd_, LOCK_EX);
      if(status < 0) return;

      status = lseek(response_fd_, 0, SEEK_SET);
      if(status < 0) {
        std::cerr << "write_response: lseek: " << strerror(errno) << std::endl;
        return;
      }
      status = ftruncate(response_fd_, 0);
      if(status < 0) {
        std::cerr << "write_response: ftruncate: " << strerror(errno) << std::endl;
        return;
      }

      ::write(response_fd_, response, size);

      flock(response_fd_, LOCK_UN);
    }

    void Console::process_responses(STATE) {
      GCTokenImpl gct;
      RBX_DTRACE_CONST char* thread_name =
        const_cast<RBX_DTRACE_CONST char*>("rbx.console.response");
      response_vm_->set_name(thread_name);

      RUBINIUS_THREAD_START(const_cast<RBX_DTRACE_CONST char*>(thread_name),
                            state->vm()->thread_id(), 1);

      state->vm()->thread->hard_unlock(state, gct, 0);
      state->gc_dependent(gct, 0);

      response_fd_ = open_file(response_path_);

      char* request = NULL;

      while(!response_exit_) {
        {
          utilities::thread::Mutex::LockGuard lg(list_lock_);
          GCIndependent guard(state, 0);

          if(request_list_->size() > 0) {
            request = request_list_->back();
            request_list_->pop_back();
          }
        }

        if(response_exit_) break;

        if(request) {
          Array* args = Array::create(state, 1);
          args->aset(state, 0, String::create(state, request));
          Object* result = console_.get()->send(state, 0, state->symbol("evaluate"),
              args, cNil);

          if(String* response = try_as<String>(result)) {
            GCIndependent guard(state, 0);

            write_response(state,
                reinterpret_cast<const char*>(response->byte_address()),
                response->byte_size());
          }

          request = NULL;
        } else {
          utilities::thread::Mutex::LockGuard lg(response_lock_);
          GCIndependent guard(state, 0);

          response_cond_.wait(response_lock_);
        }
      }

      RUBINIUS_THREAD_STOP(const_cast<RBX_DTRACE_CONST char*>(thread_name),
                           state->vm()->thread_id(), 1);
    }
  }
}
