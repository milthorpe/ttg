#ifndef PARSEC_TTG_H_INCLUDED
#define PARSEC_TTG_H_INCLUDED

/* set up env if this header was included directly */
#if !defined(TTG_IMPL_NAME)
#define TTG_USE_PARSEC 1
#endif  // !defined(TTG_IMPL_NAME)

#include "ttg/impl_selector.h"

/* include ttg header to make symbols available in case this header is included directly */
#include "../../ttg.h"

#include "ttg/base/keymap.h"
#include "ttg/base/op.h"
#include "ttg/base/world.h"
#include "ttg/edge.h"
#include "ttg/execution.h"
#include "ttg/func.h"
#include "ttg/op.h"
#include "ttg/runtimes.h"
#include "ttg/terminal.h"
#include "ttg/util/hash.h"
#include "ttg/util/meta.h"
#include "ttg/util/print.h"
#include "ttg/util/trace.h"

#include "ttg/serialization/data_descriptor.h"

#include "ttg/parsec/fwd.h"

#include <array>
#include <cassert>
#include <experimental/type_traits>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <parsec.h>
#include <parsec/class/parsec_hash_table.h>
#include <parsec/data_internal.h>
#include <parsec/execution_stream.h>
#include <parsec/interfaces/interface.h>
#include <parsec/mca/device/device.h>
#include <parsec/parsec_comm_engine.h>
#include <parsec/parsec_internal.h>
#include <parsec/scheduling.h>
#include <cstdlib>
#include <cstring>

#include <boost/callable_traits.hpp>  // needed for wrap.h

#include "ttg/parsec/ttg_data_copy.h"

/* PaRSEC function declarations */
extern "C" {
void parsec_taskpool_termination_detected(parsec_taskpool_t *tp);
int parsec_add_fetch_runtime_task(parsec_taskpool_t *tp, int tasks);
}

namespace ttg_parsec {
  static __thread parsec_task_t *parsec_ttg_caller;

  typedef void (*static_set_arg_fct_type)(void *, size_t, ttg::OpBase *);
  typedef std::pair<static_set_arg_fct_type, ttg::OpBase *> static_set_arg_fct_call_t;
  static std::map<uint64_t, static_set_arg_fct_call_t> static_id_to_op_map;
  static std::mutex static_map_mutex;
  typedef std::tuple<int, void *, size_t> static_set_arg_fct_arg_t;
  static std::multimap<uint64_t, static_set_arg_fct_arg_t> delayed_unpack_actions;

  struct msg_header_t {
    uint32_t taskpool_id;
    uint64_t op_id;
    std::size_t param_id;
    int num_keys;
  };

  namespace detail {

    static int static_unpack_msg(parsec_comm_engine_t *ce, uint64_t tag, void *data, long unsigned int size,
                                 int src_rank, void *obj) {
      static_set_arg_fct_type static_set_arg_fct;
      parsec_taskpool_t *tp = NULL;
      msg_header_t *msg = static_cast<msg_header_t *>(data);
      uint64_t op_id = msg->op_id;
      tp = parsec_taskpool_lookup(msg->taskpool_id);
      assert(NULL != tp);
      static_map_mutex.lock();
      try {
        auto op_pair = static_id_to_op_map.at(op_id);
        static_map_mutex.unlock();
        tp->tdm.module->incoming_message_start(tp, src_rank, NULL, NULL, 0, NULL);
        static_set_arg_fct = op_pair.first;
        static_set_arg_fct(data, size, op_pair.second);
        tp->tdm.module->incoming_message_end(tp, NULL);
        return 0;
      } catch (const std::out_of_range &e) {
        void *data_cpy = malloc(size);
        assert(data_cpy != 0);
        memcpy(data_cpy, data, size);
        if (ttg::tracing()) {
          ttg::print("ttg_parsec(", ttg_default_execution_context().rank(), ") Delaying delivery of message (",
                     src_rank, ", ", op_id, ", ", data_cpy, ", ", size, ")");
        }
        delayed_unpack_actions.insert(std::make_pair(op_id, std::make_tuple(src_rank, data_cpy, size)));
        static_map_mutex.unlock();
        return 1;
      }
    }

    static int get_remote_complete_cb(parsec_comm_engine_t *ce, parsec_ce_tag_t tag, void *msg, size_t msg_size,
                                      int src, void *cb_data);

    /* Helper function to properly delete a pointer, with the potential for
     * template speciailization */
    template <typename T>
    struct typed_delete_t {
      static void delete_type(void *ptr) {
        T *typed_ptr = reinterpret_cast<T *>(ptr);
        delete typed_ptr;
      }
    };

    template <typename T>
    struct typed_delete_t<std::shared_ptr<T>> {
      static void delete_type(void *ptr) {
        std::shared_ptr<T> *typed_ptr = reinterpret_cast<std::shared_ptr<T> *>(ptr);
        typed_ptr->reset();
        delete typed_ptr;
      }
    };

    inline ttg_data_copy_t *find_copy_in_task(parsec_task_t *task, const void *ptr) {
      ttg_data_copy_t *copy = nullptr;
      int j = -1;
      if (task != nullptr || ptr != nullptr) {
        while (++j < MAX_CALL_PARAM_COUNT) {
          if (NULL != task->data[j].data_in && task->data[j].data_in->device_private == ptr) {
            copy = reinterpret_cast<ttg_data_copy_t *>(task->data[j].data_in);
            break;
          }
        }
      }
      return copy;
    }

    inline bool add_copy_to_task(ttg_data_copy_t *copy, parsec_task_t *task) {
      if (task == nullptr || copy == nullptr) {
        return false;
      }

      int j = 0;
      while (j < MAX_PARAM_COUNT && nullptr != task->data[j].data_in) {
        ++j;
      }

      if (MAX_PARAM_COUNT > j) {
        PARSEC_OBJ_RETAIN(copy);
        task->data[j].data_in = copy;
        return true;
      }
      return false;
    }

    inline void remove_data_copy(ttg_data_copy_t *copy, parsec_task_t *task) {
      for (int i = 0; i < MAX_PARAM_COUNT; ++i) {
        if (copy == task->data[i].data_in) {
          task->data[i].data_in = nullptr;
          break;
        }
      }
    }

  }  // namespace detail

  class WorldImpl : public ttg::base::WorldImplBase {
    static constexpr const int _PARSEC_TTG_TAG = 10;      // This TAG should be 'allocated' at the PaRSEC level
    static constexpr const int _PARSEC_TTG_RMA_TAG = 11;  // This TAG should be 'allocated' at the PaRSEC level

    ttg::Edge<> m_ctl_edge;

   public:
    static constexpr const int PARSEC_TTG_MAX_AM_SIZE = 1024 * 1024;
    WorldImpl(int *argc, char **argv[], int ncores) {
      ttg::detail::register_world(*this);
      ctx = parsec_init(ncores, argc, argv);
      es = ctx->virtual_processes[0]->execution_streams[0];

      parsec_ce.tag_register(_PARSEC_TTG_TAG, &detail::static_unpack_msg, this, PARSEC_TTG_MAX_AM_SIZE);
      parsec_ce.tag_register(_PARSEC_TTG_RMA_TAG, &detail::get_remote_complete_cb, this, 128);

      tpool = (parsec_taskpool_t *)calloc(1, sizeof(parsec_taskpool_t));
      tpool->taskpool_id = -1;
      tpool->update_nb_runtime_task = parsec_add_fetch_runtime_task;
      tpool->taskpool_type = PARSEC_TASKPOOL_TYPE_TTG;
      parsec_taskpool_reserve_id(tpool);

#ifdef TTG_USE_USER_TERMDET
      parsec_termdet_open_module(tpool, "user_trigger");
#else   // TTG_USE_USER_TERMDET
      parsec_termdet_open_dyn_module(tpool);
#endif  // TTG_USE_USER_TERMDET
      tpool->tdm.module->monitor_taskpool(tpool, parsec_taskpool_termination_detected);
      // In TTG, we use the pending actions to denote that the
      // taskpool is not ready, i.e. some local tasks could still
      // be added by the main thread. It should then be initialized
      // to 0, execute will set it to 1 and mark the tpool as ready,
      // and the fence() will decrease it back to 0.
      tpool->tdm.module->taskpool_set_nb_pa(tpool, 0);
      parsec_taskpool_enable(tpool, NULL, NULL, es, size() > 1);

      // Termination detection in PaRSEC requires to synchronize the
      // taskpool enabling, to avoid a race condition that would keep
      // termination detection-related messages in a waiting queue
      // forever
      MPI_Barrier(comm());

      parsec_taskpool_started = false;
    }

    /* Deleted copy ctor */
    WorldImpl(const WorldImpl &other) = delete;

    /* Deleted move ctor */
    WorldImpl(WorldImpl &&other) = delete;

    /* Deleted copy assignment */
    WorldImpl &operator=(const WorldImpl &other) = delete;

    /* Deleted move assignment */
    WorldImpl &operator=(WorldImpl &&other) = delete;

    ~WorldImpl() { destroy(); }

    constexpr int parsec_ttg_tag() const { return _PARSEC_TTG_TAG; }
    constexpr int parsec_ttg_rma_tag() const { return _PARSEC_TTG_RMA_TAG; }

    virtual int size() const override {
      int size;

      MPI_Comm_size(comm(), &size);
      return size;
    }

    virtual int rank() const override {
      int rank;
      MPI_Comm_rank(comm(), &rank);
      return rank;
    }

    MPI_Comm comm() const { return MPI_COMM_WORLD; }

    virtual void execute() override {
      parsec_enqueue(ctx, tpool);
      tpool->tdm.module->taskpool_addto_nb_pa(tpool, 1);
      tpool->tdm.module->taskpool_ready(tpool);
      int ret = parsec_context_start(ctx);
      parsec_taskpool_started = true;
      if (ret != 0) throw std::runtime_error("TTG: parsec_context_start failed");
    }

    virtual void destroy() override {
      if (is_valid()) {
        release_ops();
        ttg::detail::deregister_world(*this);
        parsec_taskpool_free(tpool);
        parsec_ce.tag_unregister(_PARSEC_TTG_TAG);
        parsec_ce.tag_unregister(_PARSEC_TTG_RMA_TAG);
        parsec_fini(&ctx);
        mark_invalid();
      }
    }

    ttg::Edge<> &ctl_edge() { return m_ctl_edge; }

    const ttg::Edge<> &ctl_edge() const { return m_ctl_edge; }

    auto *context() { return ctx; }
    auto *execution_stream() { return es; }
    auto *taskpool() { return tpool; }

    void increment_created() { taskpool()->tdm.module->taskpool_addto_nb_tasks(taskpool(), 1); }
    void increment_sent_to_sched() { parsec_atomic_fetch_inc_int32(&sent_to_sched_counter()); }

    int32_t sent_to_sched() const { return this->sent_to_sched_counter(); }

    virtual void final_task() override {
#ifdef TTG_USE_USER_TERMDET
      taskpool()->tdm.module->taskpool_set_nb_tasks(taskpool(), 0);
#endif  // TTG_USE_USER_TERMDET
    }

   protected:
    virtual void fence_impl(void) override {
      int rank = this->rank();
      if (!parsec_taskpool_started) {
        if (ttg::tracing()) {
          ttg::print("ttg_parsec::(", rank, "): parsec taskpool has not been started, fence is a simple MPI_Barrier");
        }
        MPI_Barrier(ttg::get_default_world().impl().comm());
        return;
      }
      if (ttg::tracing()) {
        ttg::print("ttg_parsec::(", rank, "): parsec taskpool is ready for completion");
      }
      // We are locally ready (i.e. we won't add new tasks)
      tpool->tdm.module->taskpool_addto_nb_pa(tpool, -1);
      if (ttg::tracing()) {
        ttg::print("ttg_parsec(", rank, "): waiting for completion");
      }
      parsec_context_wait(ctx);

      // And we start again
      tpool->tdm.module->monitor_taskpool(tpool, parsec_taskpool_termination_detected);
      tpool->tdm.module->taskpool_set_nb_pa(tpool, 0);
    }

   private:
    parsec_context_t *ctx = nullptr;
    parsec_execution_stream_t *es = nullptr;
    parsec_taskpool_t *tpool = nullptr;
    bool parsec_taskpool_started = false;

    volatile int32_t &sent_to_sched_counter() const {
      static volatile int32_t sent_to_sched = 0;
      return sent_to_sched;
    }
  };

  namespace detail {
    typedef void (*parsec_static_op_t)(void *);  // static_op will be cast to this type

    typedef struct my_op_s {
      parsec_task_t parsec_task = {};
      int32_t in_data_count = 0;
      // TODO need to augment PaRSEC backend's my_op_s by stream size info, etc.  ... in_data_count will need to be
      // replaced by something like this
      //  int counter;                            // Tracks the number of arguments set
      //  std::array<std::size_t, numins> nargs;  // Tracks the number of expected values (0 = finalized)
      //  std::array<std::size_t, numins>
      //      stream_size;                        // Expected number of values to receive, only used for streaming
      //      inputs
      //  // (0 = unbounded stream)
      parsec_hash_table_item_t op_ht_item = {};
      parsec_static_op_t function_template_class_ptr[ttg::runtime_traits<ttg::Runtime::PaRSEC>::num_execution_spaces] =
          {nullptr};
      void *object_ptr = nullptr;
      void (*static_set_arg)(int, int) = nullptr;
      parsec_key_t key = 0;
      void (*deferred_release)(void *, my_op_s *) =
          nullptr;  // callback used to release the task from with the static context of complete_task_and_release
      void *op_ptr = nullptr;  // passed to deferred_release
      int *stream_size = nullptr;

      my_op_s(const int numins) {
        PARSEC_OBJ_CONSTRUCT(&this->parsec_task, parsec_task_t);
        this->stream_size = static_cast<int *>(calloc(numins, sizeof(int)));
      }

      ~my_op_s() {
        free(this->stream_size);
        PARSEC_OBJ_DESTRUCT(&this->parsec_task);
      }
    } my_op_t;

    inline parsec_hook_return_t hook(struct parsec_execution_stream_s *es, parsec_task_t *task) {
      detail::my_op_t *me = (detail::my_op_t *)task;
      me->function_template_class_ptr[static_cast<std::size_t>(ttg::ExecutionSpace::Host)](task);
      (void)es;
      return PARSEC_HOOK_RETURN_DONE;
    }
    inline parsec_hook_return_t hook_cuda(struct parsec_execution_stream_s *es, parsec_task_t *task) {
      detail::my_op_t *me = (detail::my_op_t *)task;
      me->function_template_class_ptr[static_cast<std::size_t>(ttg::ExecutionSpace::CUDA)](task);
      (void)es;
      return PARSEC_HOOK_RETURN_DONE;
    }

    inline uint64_t parsec_tasks_hash_fct(parsec_key_t key, int nb_bits, void *data) {
      /* Use all the bits of the 64 bits key, project on the lowest base bits (0 <= hash < 1024) */
      int b = 0;
      uint64_t mask = ~0ULL >> (64 - nb_bits);
      uint64_t h = (uint64_t)key;
      (void)data;
      while (b < 64) {
        b += nb_bits;
        h ^= (uint64_t)key >> b;
      }
      return (uint64_t)(h & mask);
    }

    static parsec_key_fn_t parsec_tasks_hash_fcts = {.key_equal = parsec_hash_table_generic_64bits_key_equal,
                                                     .key_print = parsec_hash_table_generic_64bits_key_print,
                                                     .key_hash = parsec_hash_table_generic_64bits_key_hash};

    template <typename KeyT, typename ValueT, typename ActivationCallbackT>
    class rma_delayed_activate {
      std::vector<KeyT> _keylist;
      ValueT _value;
      std::atomic<int> _outstanding_transfers;
      ActivationCallbackT _cb;

     public:
      rma_delayed_activate(std::vector<KeyT> &&key, ValueT &&value, int num_transfers, ActivationCallbackT cb)
          : _keylist(std::move(key)), _value(std::move(value)), _outstanding_transfers(num_transfers), _cb(cb) {}

      bool complete_transfer(void) {
        int left = --_outstanding_transfers;
        if (0 == left) {
          _cb(std::move(_keylist), std::move(_value));
          return true;
        }
        return false;
      }

      ValueT &value() { return _value; }
    };

    template <typename ActivationT>
    static int get_complete_cb(parsec_comm_engine_t *comm_engine, parsec_ce_mem_reg_handle_t lreg, ptrdiff_t ldispl,
                               parsec_ce_mem_reg_handle_t rreg, ptrdiff_t rdispl, size_t size, int remote,
                               void *cb_data) {
      parsec_ce.mem_unregister(&lreg);
      ActivationT *activation = static_cast<ActivationT *>(cb_data);
      if (activation->complete_transfer()) {
        delete activation;
      }
      return PARSEC_SUCCESS;
    }

    static int get_remote_complete_cb(parsec_comm_engine_t *ce, parsec_ce_tag_t tag, void *msg, size_t msg_size,
                                      int src, void *cb_data) {
      std::intptr_t *fn_ptr = static_cast<std::intptr_t *>(msg);
      std::function<void(void)> *fn = reinterpret_cast<std::function<void(void)> *>(*fn_ptr);
      (*fn)();
      delete fn;
      return PARSEC_SUCCESS;
    }

    template <typename FuncT>
    static int invoke_get_remote_complete_cb(parsec_comm_engine_t *ce, parsec_ce_tag_t tag, void *msg, size_t msg_size,
                                             int src, void *cb_data) {
      std::intptr_t *iptr = static_cast<std::intptr_t *>(msg);
      FuncT *fn_ptr = reinterpret_cast<FuncT *>(*iptr);
      (*fn_ptr)();
      delete fn_ptr;
      return PARSEC_SUCCESS;
    }

    inline void release_data_copy(ttg_data_copy_t *copy) {
      if (nullptr != copy) {
        if (nullptr != copy->device_private) {
          if (copy->readers > 0) {
            int32_t readers = parsec_atomic_fetch_dec_int32(&copy->readers);
            if (1 == readers) {
              copy->delete_fn(copy->device_private);
              copy->device_private = NULL;
            }
          }
        }
        if (NULL != copy->push_task) {
          /* Release the task if it was deferrred */
          parsec_task_t *push_task = copy->push_task;
          if (parsec_atomic_cas_ptr(&copy->push_task, push_task, nullptr)) {
            detail::my_op_t *deferred_op = (detail::my_op_t *)copy->push_task;
            assert(deferred_op->deferred_release);
            deferred_op->deferred_release(deferred_op->op_ptr, deferred_op);
          }
        }
        PARSEC_OBJ_RELEASE(copy);
      }
    }

    template <typename Value>
    inline ttg_data_copy_t *register_data_copy(ttg_data_copy_t *copy_in, my_op_t *task, bool readonly) {
      ttg_data_copy_t *copy_res = copy_in;
      bool replace = false;
      int32_t readers = -1;
      if (readonly && copy_in->readers > 0) {
        /* simply increment the number of readers */
        readers = parsec_atomic_fetch_inc_int32(&copy_in->readers);
      }
      if (readers < 0) {
        /* someone is going to write into this copy -> we need to make a copy */
        copy_res = NULL;
        if (readonly) {
          replace = true;
        }
      } else if (!readonly) {
        /* this task will mutate the data
         * check whether there are other readers already and potentially
         * defer the release of this task to give following readers a
         * chance to make a copy of the data before this task mutates it
         *
         * Try to replace the readers with a negative value that indicates
         * the value is mutable. If that fails we know that there are other
         * readers or writers already.
         */
        if (parsec_atomic_cas_int32(&copy_in->readers, 1, INT32_MIN)) {
          /**
           * no other readers, mark copy as mutable and defer the release
           * of the task
           */
          assert(nullptr == copy_in->push_task);
          assert(nullptr != task);
          copy_in->push_task = &task->parsec_task;
        } else {
          /* there are readers of this copy already, make a copy that we can mutate */
          copy_res = NULL;
        }
      }
      if (NULL != copy_res) {
        PARSEC_OBJ_RETAIN(copy_res);
      }

      if (NULL == copy_res) {
        ttg_data_copy_t *new_copy;
        new_copy = PARSEC_OBJ_NEW(ttg_data_copy_t);
        new_copy->device_private = (void *)(new Value(*static_cast<Value *>(copy_in->device_private)));
        new_copy->readers = 1;
        new_copy->delete_fn = &detail::typed_delete_t<Value>::delete_type;
        if (replace) {
          /* TODO: Make sure there is no race condition with the release in release_data_copy,
           * in particular when it comes to setting the callback and replacing the data */

          /* replace the task that was deferred */
          detail::my_op_t *deferred_op = (detail::my_op_t *)copy_in->push_task;
          ttg_data_copy_t *deferred_replace_copy;
          deferred_replace_copy = detail::find_copy_in_task(copy_in->push_task, copy_in->device_private);
          /* replace the copy in the deferred task */
          for (int i = 0; i < MAX_PARAM_COUNT; ++i) {
            if (copy_in->push_task->data[i].data_in == copy_in) {
              copy_in->push_task->data[i].data_in = new_copy;
              break;
            }
          }
          assert(deferred_op->deferred_release);
          deferred_op->deferred_release(deferred_op->op_ptr, deferred_op);
          copy_in->push_task = NULL;
          copy_in->readers = 1;  // set the copy back to being read-only
          ++copy_in->readers;    // register as reader
          copy_res = copy_in;    // return the copy we were passed
        } else {
          copy_res = new_copy;  // return the new copy
        }
      }
      return copy_res;
    }
  }  // namespace detail

  template <typename... RestOfArgs>
  inline void ttg_initialize(int argc, char **argv, int taskpool_size, RestOfArgs &&...) {
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);

    auto world_ptr = new ttg_parsec::WorldImpl{&argc, &argv, taskpool_size};
    std::shared_ptr<ttg::base::WorldImplBase> world_sptr{static_cast<ttg::base::WorldImplBase *>(world_ptr)};
    ttg::World world{std::move(world_sptr)};
    ttg::detail::set_default_world(std::move(world));
  }
  inline void ttg_finalize() {
    ttg::detail::set_default_world(ttg::World{});  // reset the default world
    ttg::detail::destroy_worlds<ttg_parsec::WorldImpl>();
    MPI_Finalize();
  }
  inline void ttg_abort() { MPI_Abort(ttg::get_default_world().impl().comm(), 1); }
  inline ttg::World ttg_default_execution_context() { return ttg::get_default_world(); }
  inline void ttg_execute(ttg::World world) { world.impl().execute(); }
  inline void ttg_fence(ttg::World world) { world.impl().fence(); }

  template <typename T>
  inline void ttg_register_ptr(ttg::World world, const std::shared_ptr<T> &ptr) {
    world.impl().register_ptr(ptr);
  }

  inline void ttg_register_status(ttg::World world, const std::shared_ptr<std::promise<void>> &status_ptr) {
    world.impl().register_status(status_ptr);
  }

  inline ttg::Edge<> &ttg_ctl_edge(ttg::World world) { return world.impl().ctl_edge(); }

  inline void ttg_sum(ttg::World world, double &value) {
    double result = 0.0;
    MPI_Allreduce(&value, &result, 1, MPI_DOUBLE, MPI_SUM, world.impl().comm());
    value = result;
  }

  /// broadcast
  /// @tparam T a serializable type
  template <typename T>
  void ttg_broadcast(::ttg::World world, T &data, int source_rank) {
    int64_t BUFLEN;
    if (world.rank() == source_rank) {
      BUFLEN = ttg::default_data_descriptor<T>::payload_size(&data);
    }
    MPI_Bcast(&BUFLEN, 1, MPI_INT64_T, source_rank, world.impl().comm());

    unsigned char *buf = new unsigned char[BUFLEN];
    if (world.rank() == source_rank) {
      ttg::default_data_descriptor<T>::pack_payload(&data, BUFLEN, 0, buf);
    }
    MPI_Bcast(buf, BUFLEN, MPI_UNSIGNED_CHAR, source_rank, world.impl().comm());
    if (world.rank() != source_rank) {
      ttg::default_data_descriptor<T>::unpack_payload(&data, BUFLEN, 0, buf);
    }
    delete[] buf;
  }

  namespace detail {

    struct ParsecBaseOp {
     protected:
      //  static std::map<int, ParsecBaseOp*> function_id_to_instance;
      parsec_hash_table_t tasks_table;
      parsec_task_class_t self;
    };

    struct msg_t {
      msg_header_t op_id;
      unsigned char bytes[WorldImpl::PARSEC_TTG_MAX_AM_SIZE - sizeof(msg_header_t)];

      msg_t() = default;
      msg_t(uint64_t op_id, uint32_t taskpool_id, std::size_t param_id, int num_keys = 1)
          : op_id{taskpool_id, op_id, param_id, num_keys} {}
    };
  }  // namespace detail

  template <typename keyT, typename output_terminalsT, typename derivedT, typename... input_valueTs>
  class Op : public ttg::OpBase, detail::ParsecBaseOp {
   private:
    using opT = Op<keyT, output_terminalsT, derivedT, input_valueTs...>;
    parsec_mempool_t mempools;
    std::map<std::pair<int, int>, int> mempools_index;

    // check for a non-type member named have_cuda_op
    template <typename T>
    using have_cuda_op_non_type_t = decltype(&T::have_cuda_op);

    bool alive = true;

   public:
    static constexpr int numins = sizeof...(input_valueTs);                    // number of input arguments
    static constexpr int numouts = std::tuple_size<output_terminalsT>::value;  // number of outputs

    /// @return true if derivedT::have_cuda_op exists and is defined to true
    static constexpr bool derived_has_cuda_op() {
      if constexpr (std::experimental::is_detected_v<have_cuda_op_non_type_t, derivedT>) {
        return derivedT::have_cuda_op;
      } else {
        return false;
      }
    }

    using input_terminals_type = std::tuple<ttg::In<keyT, input_valueTs>...>;
    using input_args_type = std::tuple<input_valueTs...>;
    using input_edges_type = std::tuple<ttg::Edge<keyT, std::decay_t<input_valueTs>>...>;
    static_assert(ttg::meta::is_none_Void_v<input_valueTs...>, "ttg::Void is for internal use only, do not use it");
    // if have data inputs and (always last) control input, convert last input to Void to make logic easier
    using input_values_full_tuple_type = std::tuple<ttg::meta::void_to_Void_t<std::decay_t<input_valueTs>>...>;
    using input_refs_full_tuple_type =
        std::tuple<std::add_lvalue_reference_t<ttg::meta::void_to_Void_t<input_valueTs>>...>;
    using input_values_tuple_type =
        std::conditional_t<ttg::meta::is_none_void_v<input_valueTs...>, input_values_full_tuple_type,
                           typename ttg::meta::drop_last_n<input_values_full_tuple_type, std::size_t{1}>::type>;
    using input_refs_tuple_type =
        std::conditional_t<ttg::meta::is_none_void_v<input_valueTs...>, input_refs_full_tuple_type,
                           typename ttg::meta::drop_last_n<input_refs_full_tuple_type, std::size_t{1}>::type>;
    using input_unwrapped_values_tuple_type = input_values_tuple_type;
    static constexpr int numinvals =
        std::tuple_size_v<input_refs_tuple_type>;  // number of input arguments with values (i.e. omitting the control
                                                   // input, if any)

    using output_terminals_type = output_terminalsT;
    using output_edges_type = typename ttg::terminals_to_edges<output_terminalsT>::type;

    template <std::size_t i, typename resultT, typename InTuple>
    static resultT get(InTuple &&intuple) {
      return static_cast<resultT>(std::get<i>(std::forward<InTuple>(intuple)));
    };
    template <std::size_t i, typename InTuple>
    static auto &get(InTuple &&intuple) {
      return std::get<i>(std::forward<InTuple>(intuple));
    };

   private:
    input_terminals_type input_terminals;
    output_terminalsT output_terminals;
    std::array<void (Op::*)(void *, std::size_t), numins> set_arg_from_msg_fcts;

    ttg::World world;
    ttg::meta::detail::keymap_t<keyT> keymap;
    ttg::meta::detail::keymap_t<keyT> priomap;
    // For now use same type for unary/streaming input terminals, and stream reducers assigned at runtime
    ttg::meta::detail::input_reducers_t<input_valueTs...>
        input_reducers;  //!< Reducers for the input terminals (empty = expect single value)

   public:
    ttg::World get_world() const { return world; }

   private:
    template <std::size_t... IS>
    static auto make_set_args_fcts(std::index_sequence<IS...>) {
      using resultT = decltype(set_arg_from_msg_fcts);
      return resultT{{&Op::set_arg_from_msg<IS>...}};
    }

    /// dispatches a call to derivedT::op if Space == Host, otherwise to derivedT::op_cuda if Space == CUDA
    template <ttg::ExecutionSpace Space, typename... Args>
    void op(Args &&... args) {
      derivedT *derived = static_cast<derivedT *>(this);
      if constexpr (Space == ttg::ExecutionSpace::Host)
        derived->op(std::forward<Args>(args)...);
      else if constexpr (Space == ttg::ExecutionSpace::CUDA)
        derived->op_cuda(std::forward<Args>(args)...);
      else
        abort();
    }

    template <std::size_t... IS>
    static input_refs_tuple_type make_tuple_of_ref_from_array(detail::my_op_t *task, std::index_sequence<IS...>) {
      return input_refs_tuple_type{static_cast<typename std::tuple_element<IS, input_refs_tuple_type>::type>(
          *reinterpret_cast<std::remove_reference_t<typename std::tuple_element<IS, input_refs_tuple_type>::type> *>(
              task->parsec_task.data[IS].data_in->device_private))...};
    }

    template <ttg::ExecutionSpace Space>
    static void static_op(parsec_task_t *my_task) {
      detail::my_op_t *task = (detail::my_op_t *)my_task;
      opT *baseobj = (opT *)task->object_ptr;
      derivedT *obj = (derivedT *)task->object_ptr;
      assert(parsec_ttg_caller == NULL);
      parsec_ttg_caller = my_task;
      if (obj->tracing()) {
        if constexpr (!ttg::meta::is_void_v<keyT>)
          ttg::print(obj->get_world().rank(), ":", obj->get_name(), " : ", *(keyT *)task->key, ": executing");
        else
          ttg::print(obj->get_world().rank(), ":", obj->get_name(), " : executing");
      }

      if constexpr (!ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_values_tuple_type>) {
        input_refs_tuple_type input = make_tuple_of_ref_from_array(task, std::make_index_sequence<numinvals>{});
        baseobj->template op<Space>(*(keyT *)task->key, std::move(input), obj->output_terminals);
      } else if constexpr (!ttg::meta::is_void_v<keyT> && ttg::meta::is_empty_tuple_v<input_values_tuple_type>) {
        baseobj->template op<Space>(*(keyT *)task->key, obj->output_terminals);
      } else if constexpr (ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_values_tuple_type>) {
        input_refs_tuple_type input = make_tuple_of_ref_from_array(task, std::make_index_sequence<numinvals>{});
        baseobj->template op<Space>(std::move(input), obj->output_terminals);
      } else if constexpr (ttg::meta::is_void_v<keyT> && ttg::meta::is_empty_tuple_v<input_values_tuple_type>) {
        baseobj->template op<Space>(obj->output_terminals);
      } else
        abort();
      parsec_ttg_caller = NULL;

      if (obj->tracing()) {
        if constexpr (!ttg::meta::is_void_v<keyT>)
          ttg::print(obj->get_world().rank(), ":", obj->get_name(), " : ", *(keyT *)task->key, ": done executing");
        else
          ttg::print(obj->get_world().rank(), ":", obj->get_name(), " : done executing");
      }
    }

    template <ttg::ExecutionSpace Space>
    static void static_op_noarg(parsec_task_t *my_task) {
      detail::my_op_t *task = (detail::my_op_t *)my_task;
      opT *baseobj = (opT *)task->object_ptr;
      derivedT *obj = (derivedT *)task->object_ptr;
      assert(parsec_ttg_caller == NULL);
      parsec_ttg_caller = my_task;
      if constexpr (!ttg::meta::is_void_v<keyT>) {
        baseobj->template op<Space>(*(keyT *)task->key, obj->output_terminals);
      } else if constexpr (ttg::meta::is_void_v<keyT>) {
        baseobj->template op<Space>(obj->output_terminals);
      } else
        abort();
      parsec_ttg_caller = NULL;
    }

   protected:
    template <typename T>
    uint64_t unpack(T &obj, void *_bytes, uint64_t pos) {
      const ttg_data_descriptor *dObj = ttg::get_data_descriptor<ttg::meta::remove_cvr_t<T>>();
      uint64_t payload_size;
      if constexpr (!ttg::default_data_descriptor<ttg::meta::remove_cvr_t<T>>::serialize_size_is_const) {
        const ttg_data_descriptor *dSiz = ttg::get_data_descriptor<uint64_t>();
        dSiz->unpack_payload(&payload_size, sizeof(uint64_t), pos, _bytes);
        pos += sizeof(uint64_t);
      } else {
        payload_size = dObj->payload_size(&obj);
      }
      dObj->unpack_payload(&obj, payload_size, pos, _bytes);
      return pos + payload_size;
    }

    template <typename T>
    uint64_t pack(T &obj, void *bytes, uint64_t pos) {
      const ttg_data_descriptor *dObj = ttg::get_data_descriptor<ttg::meta::remove_cvr_t<T>>();
      uint64_t payload_size = dObj->payload_size(&obj);
      if constexpr (!ttg::default_data_descriptor<ttg::meta::remove_cvr_t<T>>::serialize_size_is_const) {
        const ttg_data_descriptor *dSiz = ttg::get_data_descriptor<uint64_t>();
        dSiz->pack_payload(&payload_size, sizeof(uint64_t), pos, bytes);
        pos += sizeof(uint64_t);
      }
      dObj->pack_payload(&obj, payload_size, pos, bytes);
      return pos + payload_size;
    }

    static void static_set_arg(void *data, std::size_t size, ttg::OpBase *bop) {
      assert(size >= sizeof(msg_header_t) &&
             "Trying to unpack as message that does not hold enough bytes to represent a single header");
      msg_header_t *hd = static_cast<msg_header_t *>(data);
      derivedT *obj = reinterpret_cast<derivedT *>(bop);
      if (-1 != hd->param_id) {
        auto member = obj->set_arg_from_msg_fcts[hd->param_id];
        (obj->*member)(data, size);
      } else {
        if constexpr (ttg::meta::is_empty_tuple_v<input_refs_tuple_type>) {
          if constexpr (ttg::meta::is_void_v<keyT>) {
            obj->template set_arg<keyT>();
          } else {
            using msg_t = detail::msg_t;
            msg_t *msg = static_cast<msg_t *>(data);
            keyT key;
            obj->unpack(key, static_cast<void *>(msg->bytes), 0);
            obj->template set_arg<keyT>(key);
          }
        } else {
          abort();
        }
      }
    }

    template <size_t i, typename valueT>
    void set_arg_from_msg_keylist(ttg::span<keyT> &&keylist, valueT &&value) {
      /* create a dummy task that holds the copy, which can be reused by others */
      detail::my_op_t *dummy;
      parsec_execution_stream_s *es = world.impl().execution_stream();
      parsec_thread_mempool_t *mempool =
          &mempools.thread_mempools[mempools_index[std::pair<int, int>(es->virtual_process->vp_id, es->th_id)]];
      dummy = (detail::my_op_t *)parsec_thread_mempool_allocate(mempool);
      memset((void *)dummy, 0, sizeof(detail::my_op_t));
      dummy->parsec_task.mempool_owner = mempool;
      PARSEC_OBJ_CONSTRUCT(&dummy->parsec_task, parsec_list_item_t);
      dummy->parsec_task.task_class = &this->self;

      /* set the received value as the dummy's only data */
      using decay_valueT = std::decay_t<valueT>;
      auto *val_copy = new decay_valueT(std::move(value));
      ttg_data_copy_t *copy = PARSEC_OBJ_NEW(ttg_data_copy_t);
      copy->device_private = val_copy;
      copy->readers = 1;
      copy->delete_fn = &detail::typed_delete_t<decay_valueT>::delete_type;
      dummy->parsec_task.data[0].data_in = copy;

      /* save the current task and set the dummy task */
      auto parsec_ttg_caller_save = parsec_ttg_caller;
      parsec_ttg_caller = &dummy->parsec_task;

      /* iterate over the keys and have them use the copy we made */
      for (auto key : keylist) {
        set_arg<i, keyT, valueT>(key, *val_copy);
      }

      /* restore the previous task */
      parsec_ttg_caller = parsec_ttg_caller_save;

      /* release the dummy task */
      complete_task_and_release(es, &dummy->parsec_task);
    }

    // there are 6 types of set_arg:
    // - case 1: nonvoid Key, complete Value type
    // - case 2: nonvoid Key, void Value, mixed (data+control) inputs
    // - case 3: nonvoid Key, void Value, no inputs
    // - case 4:    void Key, complete Value type
    // - case 5:    void Key, void Value, mixed (data+control) inputs
    // - case 6:    void Key, void Value, no inputs
    // implementation of these will be further split into "local-only" and global+local

    template <std::size_t i>
    void set_arg_from_msg(void *data, std::size_t size) {
      using valueT = typename std::tuple_element<i, input_terminals_type>::type::value_type;
      using msg_t = detail::msg_t;
      msg_t *msg = static_cast<msg_t *>(data);
      if constexpr (!ttg::meta::is_void_v<keyT>) {
        /* unpack the keys */
        uint64_t pos = 0;
        std::vector<keyT> keylist;
        int num_keys = msg->op_id.num_keys;
        keylist.reserve(num_keys);
        auto rank = world.rank();
        for (int k = 0; k < num_keys; ++k) {
          keyT key;
          pos = unpack(key, msg->bytes, pos);
          assert(keymap(key) == rank);
          keylist.push_back(std::move(key));
        }
        // case 1
        if constexpr (!ttg::meta::is_empty_tuple_v<input_refs_tuple_type> && !std::is_void_v<valueT>) {
          using decvalueT = std::decay_t<valueT>;
          if constexpr (!ttg::has_split_metadata<decvalueT>::value) {
            decvalueT val;
            unpack(val, msg->bytes, pos);

            set_arg_from_msg_keylist<i>(ttg::span<keyT>(&keylist[0], num_keys), std::move(val));
          } else {
            /* unpack the header and start the RMA transfers */
            ttg::SplitMetadataDescriptor<decvalueT> descr;
            using metadata_t = decltype(descr.get_metadata(std::declval<decvalueT>()));
            size_t metadata_size = sizeof(metadata_t);

            /* unpack the metadata */
            metadata_t metadata;
            std::memcpy(&metadata, msg->bytes + pos, metadata_size);
            pos += metadata_size;

            /* unpack the remote rank */
            int remote;
            std::memcpy(&remote, msg->bytes + pos, sizeof(remote));
            pos += sizeof(remote);

            assert(remote < world.size());

            /* extract the number of chunks */
            int32_t num_iovecs;
            std::memcpy(&num_iovecs, msg->bytes + pos, sizeof(num_iovecs));
            pos += sizeof(num_iovecs);

            /* extract the callback tag */
            parsec_ce_tag_t cbtag;
            std::memcpy(&cbtag, msg->bytes + pos, sizeof(cbtag));
            pos += sizeof(cbtag);

            /* create the value from the metadata */
            auto activation =
                new detail::rma_delayed_activate(std::move(keylist), descr.create_from_metadata(metadata), num_iovecs,
                                                 [this, num_keys](std::vector<keyT> &&keylist, valueT &&value) {
                                                   set_arg_from_msg_keylist<i>(keylist, value);
                                                 });
            auto &val = activation->value();

            using ActivationT = std::decay_t<decltype(*activation)>;

            int nv = 0;
            /* process payload iovecs */
            auto iovecs = descr.get_data(val);
            /* start the RMA transfers */
            for (auto iov : iovecs) {
              ++nv;
              parsec_ce_mem_reg_handle_t rreg;
              int32_t rreg_size_i;
              std::memcpy(&rreg_size_i, msg->bytes + pos, sizeof(rreg_size_i));
              pos += sizeof(rreg_size_i);
              rreg = static_cast<parsec_ce_mem_reg_handle_t>(msg->bytes + pos);
              pos += rreg_size_i;
              // std::intptr_t *fn_ptr = reinterpret_cast<std::intptr_t *>(msg->bytes + pos);
              // pos += sizeof(*fn_ptr);
              std::intptr_t fn_ptr;
              std::memcpy(&fn_ptr, msg->bytes + pos, sizeof(fn_ptr));
              pos += sizeof(fn_ptr);

              /* register the local memory */
              parsec_ce_mem_reg_handle_t lreg;
              size_t lreg_size;
              parsec_ce.mem_register(iov.data, PARSEC_MEM_TYPE_NONCONTIGUOUS, iov.num_bytes, parsec_datatype_int8_t,
                                     iov.num_bytes, &lreg, &lreg_size);
              /* TODO: PaRSEC should treat the remote callback as a tag, not a function pointer! */
              parsec_ce.get(&parsec_ce, lreg, 0, rreg, 0, iov.num_bytes, remote, &detail::get_complete_cb<ActivationT>,
                            activation,
                            /*world.impl().parsec_ttg_rma_tag()*/
                            cbtag, &fn_ptr, sizeof(std::intptr_t));
            }

            assert(num_iovecs == nv);
            assert(size == (pos + sizeof(msg_header_t)));
          }
          // case 2
        } else if constexpr (!ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                             std::is_void_v<valueT>) {
          for (auto key : keylist) {
            set_arg<i, keyT, ttg::Void>(key, ttg::Void{});
          }
          // case 3
        } else if constexpr (!ttg::meta::is_void_v<keyT> && ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                             std::is_void_v<valueT>) {
          for (auto key : keylist) {
            set_arg<keyT>(key);
          }
        }
        // case 4
      } else if constexpr (ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                           !std::is_void_v<valueT>) {
        using decvalueT = std::decay_t<valueT>;
        decvalueT val;
        unpack(val, msg->bytes, 0);
        set_arg<i, keyT, valueT>(std::move(val));
        // case 5
      } else if constexpr (ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                           std::is_void_v<valueT>) {
        set_arg<i, keyT, ttg::Void>(ttg::Void{});
        // case 6
      } else if constexpr (ttg::meta::is_void_v<keyT> && ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                           std::is_void_v<valueT>) {
        set_arg<keyT>();
      } else {
        abort();
      }
    }

    template <std::size_t i, typename Key, typename Value>
    std::enable_if_t<!ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg_local(
        const Key &key, Value &&value) {
      set_arg_local_impl<i>(key, std::forward<Value>(value));
    }

    template <std::size_t i, typename Key = keyT, typename Value>
    std::enable_if_t<ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg_local(
        Value &&value) {
      set_arg_local_impl<i>(ttg::Void{}, std::forward<Value>(value));
    }

    template <std::size_t i, typename Key, typename Value>
    std::enable_if_t<!ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg_local(
        const Key &key, const Value &value) {
      set_arg_local_impl<i>(key, value);
    }

    template <std::size_t i, typename Key = keyT, typename Value>
    std::enable_if_t<ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg_local(
        const Value &value) {
      set_arg_local_impl<i>(ttg::Void{}, value);
    }

    template <std::size_t i, typename Key = keyT, typename Value>
    std::enable_if_t<ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg_local(
        std::shared_ptr<const Value> &valueptr) {
      set_arg_local_impl<i>(ttg::Void{}, *valueptr);
    }

    template <typename Key>
    detail::my_op_t *newtask_with_lock(const Key &key) {
      constexpr const bool keyT_is_Void = ttg::meta::is_void_v<Key>;
      auto &world_impl = world.impl();
      detail::my_op_t *newtask;
      parsec_execution_stream_s *es = world_impl.execution_stream();
      parsec_thread_mempool_t *mempool =
          &mempools.thread_mempools[mempools_index[std::pair<int, int>(es->virtual_process->vp_id, es->th_id)]];
      void *newtask_ptr = parsec_thread_mempool_allocate(mempool);
      newtask = new (newtask_ptr) detail::my_op_t(numins);  // placement new
      newtask->parsec_task.mempool_owner = mempool;

      newtask->parsec_task.task_class = &this->self;
      newtask->parsec_task.taskpool = world_impl.taskpool();
      newtask->parsec_task.status = PARSEC_TASK_STATUS_HOOK;
      if constexpr (!keyT_is_Void) {
        newtask->parsec_task.priority = priomap(key);
      } else {
        newtask->parsec_task.priority = priomap();
      }

      newtask->function_template_class_ptr[static_cast<std::size_t>(ttg::ExecutionSpace::Host)] =
          reinterpret_cast<detail::parsec_static_op_t>(&Op::static_op<ttg::ExecutionSpace::Host>);
      if constexpr (derived_has_cuda_op())
        newtask->function_template_class_ptr[static_cast<std::size_t>(ttg::ExecutionSpace::CUDA)] =
            reinterpret_cast<detail::parsec_static_op_t>(&Op::static_op<ttg::ExecutionSpace::CUDA>);
      newtask->object_ptr = static_cast<derivedT *>(this);
      if constexpr (ttg::meta::is_void_v<keyT>) {
        newtask->key = 0;
      } else {
        keyT *new_key = new keyT(key);
        newtask->key = reinterpret_cast<parsec_key_t>(new_key);
      }

      if (tracing()) ttg::print(world.rank(), ":", get_name(), " : ", key, ": creating task");
      newtask->op_ht_item.key = newtask->key;
      world_impl.increment_created();
      parsec_hash_table_nolock_insert(&tasks_table, &newtask->op_ht_item);
      return newtask;
    }

    // Used to set the i'th argument
    template <std::size_t i, typename Key, typename Value>
    void set_arg_local_impl(const Key &key, Value &&value) {
      using valueT = typename std::tuple_element<i, input_values_full_tuple_type>::type;
      constexpr const bool valueT_is_Void = ttg::meta::is_void_v<valueT>;
      constexpr const bool keyT_is_Void = ttg::meta::is_void_v<Key>;

      if (tracing()) {
        if constexpr (!valueT_is_Void) {
          ttg::print(world.rank(), ":", get_name(), " : ", key, ": received value for argument : ", i,
                     " : value = ", value);
        } else {
          ttg::print(world.rank(), ":", get_name(), " : ", key, ": received value for argument : ", i);
        }
      }

      if constexpr (!keyT_is_Void) {
        assert(keymap(key) == world.rank());
      }

      auto hk = reinterpret_cast<parsec_key_t>(&key);
      detail::my_op_t *task = nullptr;
      auto &world_impl = world.impl();
      parsec_hash_table_lock_bucket(&tasks_table, hk);
      if (nullptr == (task = (detail::my_op_t *)parsec_hash_table_nolock_find(&tasks_table, hk))) {
        task = newtask_with_lock(key);
      }
      parsec_hash_table_unlock_bucket(&tasks_table, hk);

      /* whether the task needs to be deferred or not */
      bool needs_deferring = false;

      constexpr const bool input_is_const = std::is_const_v<std::tuple_element_t<i, input_args_type>>;
      ttg_data_copy_t *copy = nullptr;

      auto reducer = std::get<i>(input_reducers);
      if (reducer) {  // is this a streaming input? reduce the received value
        // N.B. Right now reductions are done eagerly, without spawning tasks
        //      this means we must lock
        parsec_hash_table_lock_bucket(&tasks_table, hk);
        bool release = false;

        if constexpr (!ttg::meta::is_void_v<valueT>) {  // for data values
          // have a value already? if not, set, otherwise reduce
          if (nullptr == (copy = reinterpret_cast<ttg_data_copy_t *>(task->parsec_task.data[i].data_in))) {
            using decay_valueT = std::decay_t<valueT>;
            auto *val_copy = new decay_valueT(std::move(value));
            copy = PARSEC_OBJ_NEW(ttg_data_copy_t);
            copy->device_private = val_copy;
            copy->readers = 1;
            copy->delete_fn = &detail::typed_delete_t<decay_valueT>::delete_type;
            task->parsec_task.data[i].data_in = copy;
          } else {
            // TODO: Ask Ed -- Why do we need a copy of value here?
            valueT value_copy = value;  // use constexpr if to avoid making a copy if given nonconst rvalue
            *reinterpret_cast<std::decay_t<valueT> *>(copy->device_private) = std::move(
                reducer(reinterpret_cast<std::decay_t<valueT>&&>(*reinterpret_cast<std::decay_t<valueT>*>(copy->device_private)), std::move(value_copy)));
            task->stream_size[i]--;
            release = (task->stream_size[i] == 0);
          }
        } else {
          reducer();  // even if this was a control input, must execute the reducer for possible side effects
          task->stream_size[i]--;
          release = (task->stream_size[i] == 0);
        }
        parsec_hash_table_unlock_bucket(&tasks_table, hk);
        if (release) release_task(this, task);
      } else {
        if constexpr (!valueT_is_Void) {
          if (nullptr != task->parsec_task.data[i].data_in) {
            ttg::print_error(get_name(), " : ", key, ": error argument is already set : ", i);
            throw std::logic_error("bad set arg");
          }

          if (nullptr != parsec_ttg_caller) {
            copy = detail::find_copy_in_task(parsec_ttg_caller, &value);
          }

          if (nullptr != copy) {
            /* register_data_copy might provide us with a different copy if !input_is_const */
            copy = detail::register_data_copy<valueT>(copy, task, input_is_const);
            /* if we registered as a writer and were the first to register with this copy
             * we need to defer the release of this task to give other tasks a chance to
             * make a copy of the original data */
            needs_deferring = (copy->readers < 0);
          } else {
            using decay_valueT = std::decay_t<valueT>;
            auto *val_copy = new decay_valueT(std::move(value));
            copy = PARSEC_OBJ_NEW(ttg_data_copy_t);
            copy->device_private = val_copy;
            copy->readers = 1;
            copy->delete_fn = &detail::typed_delete_t<decay_valueT>::delete_type;
          }
          task->parsec_task.data[i].data_in = copy;
        }
        if (needs_deferring) {
          if (nullptr == task->deferred_release) {
            task->deferred_release = &release_task;
            task->op_ptr = this;
          }
        } else {
          release_task(this, task);
        }
      }
    }

    static void release_task(void *op_ptr, detail::my_op_t *task) {
      opT &op = *reinterpret_cast<opT *>(op_ptr);
      int32_t count = parsec_atomic_fetch_inc_int32(&task->in_data_count) + 1;
      assert(count <= op.self.dependencies_goal);
      auto &world_impl = op.world.impl();

      if (count == op.self.dependencies_goal) {
        /* reset the reader counters of all mutable copies to 1 */
        for (int j = 0; j < task->parsec_task.task_class->nb_flows; j++) {
          if (nullptr != task->parsec_task.data[j].data_in && task->parsec_task.data[j].data_in->readers < 0) {
            task->parsec_task.data[j].data_in->readers = 1;
          }
        }

        world_impl.increment_sent_to_sched();
        parsec_execution_stream_t *es = world_impl.execution_stream();
        parsec_key_t hk = reinterpret_cast<parsec_key_t>(task->key);
        if (op.tracing())
          ttg::print(op.world.rank(), ":", op.get_name(), " : ", task->key, ": submitting task for op ");
        parsec_hash_table_remove(&op.tasks_table, hk);
        __parsec_schedule(es, &task->parsec_task, 0);
      }
    }

    // cases 1+2
    template <std::size_t i, typename Key, typename Value>
    std::enable_if_t<!ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg(const Key &key,
                                                                                                       Value &&value) {
      set_arg_impl<i>(key, std::forward<Value>(value), true);
    }

    template <std::size_t i, typename Key, typename Value>
    std::enable_if_t<!ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg(
        const Key &key, const Value &value) {
      set_arg_impl<i>(key, value, false);
    }

    // cases 4+5
    template <std::size_t i, typename Key, typename Value>
    std::enable_if_t<ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg(Value &&value) {
      set_arg_impl<i>(ttg::Void{}, std::forward<Value>(value), true);
    }

    template <std::size_t i, typename Key, typename Value>
    std::enable_if_t<ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>>, void> set_arg(
        const Value &value) {
      set_arg_impl<i>(ttg::Void{}, std::forward<Value>(value), false);
    }

    // Used to set the i'th argument
    template <std::size_t i, typename Key, typename Value>
    void set_arg_impl(const Key &key, Value &&value, bool is_move) {
      using valueT = typename std::tuple_element<i, input_values_full_tuple_type>::type;
      int owner;

      if constexpr (!ttg::meta::is_void_v<Key>)
        owner = keymap(key);
      else
        owner = keymap();
      if (owner == world.rank()) {
        if constexpr (!ttg::meta::is_void_v<keyT>)
          set_arg_local<i, keyT, Value>(key, std::forward<Value>(value));
        else
          set_arg_local<i, keyT, Value>(std::forward<Value>(value));
        return;
      }
      // the target task is remote. Pack the information and send it to
      // the corresponding peer.
      // TODO do we need to copy value?
      using msg_t = detail::msg_t;
      auto &world_impl = world.impl();
      uint64_t pos = 0;
      std::unique_ptr<msg_t> msg = std::make_unique<msg_t>(get_instance_id(), world_impl.taskpool()->taskpool_id, i, 1);
      using decvalueT = std::decay_t<Value>;
      /* pack the key */
      pos = pack(key, msg->bytes, pos);
      msg->op_id.num_keys = 1;
      if constexpr (!ttg::has_split_metadata<decvalueT>::value) {
        // std::cout << "set_arg_from_msg unpacking from offset " << sizeof(keyT) << std::endl;
        pos = pack(value, msg->bytes, pos);
      } else {
        ttg_data_copy_t *copy;
        copy = detail::find_copy_in_task(parsec_ttg_caller, &value);
        copy = detail::register_data_copy<decvalueT>(copy, nullptr, true);

        ttg::SplitMetadataDescriptor<decvalueT> descr;
        auto metadata = descr.get_metadata(value);
        size_t metadata_size = sizeof(metadata);
        /* pack the metadata */
        std::memcpy(msg->bytes + pos, &metadata, metadata_size);
        pos += metadata_size;
        /* pack the local rank */
        int rank = world.rank();
        std::memcpy(msg->bytes + pos, &rank, sizeof(rank));
        pos += sizeof(rank);

        auto iovecs = descr.get_data(*static_cast<decvalueT *>(copy->device_private));

        int32_t num_iovs = std::distance(std::begin(iovecs), std::end(iovecs));
        std::memcpy(msg->bytes + pos, &num_iovs, sizeof(num_iovs));
        pos += sizeof(num_iovs);

        /* TODO: at the moment, the tag argument to parsec_ce.get() is treated as a
         * raw function pointer instead of a preregistered AM tag, so play that game.
         * Once this is fixed in PaRSEC we need to use parsec_ttg_rma_tag instead! */
        parsec_ce_tag_t cbtag = reinterpret_cast<parsec_ce_tag_t>(&detail::get_remote_complete_cb);
        std::memcpy(msg->bytes + pos, &cbtag, sizeof(cbtag));
        pos += sizeof(cbtag);

        /**
         * register the generic iovecs and pack the registration handles
         * memory layout: [<lreg_size, lreg, release_cb_ptr>, ...]
         */
        for (auto iov : iovecs) {
          parsec_ce_mem_reg_handle_t lreg;
          size_t lreg_size;
          /* TODO: only register once when we can broadcast the data! */
          parsec_ce.mem_register(iov.data, PARSEC_MEM_TYPE_NONCONTIGUOUS, iov.num_bytes, parsec_datatype_int8_t,
                                 iov.num_bytes, &lreg, &lreg_size);
          auto lreg_ptr = std::shared_ptr<void>{lreg, [](void *ptr) {
                                                  parsec_ce_mem_reg_handle_t memreg = (parsec_ce_mem_reg_handle_t)ptr;
                                                  parsec_ce.mem_unregister(&memreg);
                                                }};
          int32_t lreg_size_i = lreg_size;
          std::memcpy(msg->bytes + pos, &lreg_size_i, sizeof(lreg_size_i));
          pos += sizeof(lreg_size_i);
          std::memcpy(msg->bytes + pos, lreg, lreg_size_i);
          pos += lreg_size_i;
          /* TODO: can we avoid the extra indirection of going through std::function? */
          std::function<void(void)> *fn = new std::function<void(void)>([=]() mutable {
            /* shared_ptr of value and registration captured by value so resetting
             * them here will eventually release the memory/registration */
            detail::release_data_copy(copy);
            lreg_ptr.reset();
          });
          std::intptr_t fn_ptr{reinterpret_cast<std::intptr_t>(fn)};
          std::memcpy(msg->bytes + pos, &fn_ptr, sizeof(fn_ptr));
          pos += sizeof(fn_ptr);
        }
      }
      parsec_taskpool_t *tp = world_impl.taskpool();
      tp->tdm.module->outgoing_message_start(tp, owner, NULL);
      tp->tdm.module->outgoing_message_pack(tp, owner, NULL, NULL, 0);
      // std::cout << "Sending AM with " << msg->op_id.num_keys << " keys " << std::endl;
      parsec_ce.send_am(&parsec_ce, world_impl.parsec_ttg_tag(), owner, static_cast<void *>(msg.get()),
                        sizeof(msg_header_t) + pos);
    }

    // case 3
    template <typename Key = keyT>
    std::enable_if_t<!ttg::meta::is_void_v<Key>, void> set_arg(const Key &key) {
      static_assert(ttg::meta::is_empty_tuple_v<input_refs_tuple_type>,
                    "logic error: set_arg (case 3) called but input_refs_tuple_type is nonempty");

      const auto owner = keymap(key);
      auto &world_impl = world.impl();
      if (owner == world.rank()) {
        // create PaRSEC task
        // and give it to the scheduler
        detail::my_op_t *task;
        parsec_execution_stream_s *es = world_impl.execution_stream();
        parsec_thread_mempool_t *mempool =
            &mempools.thread_mempools[mempools_index[std::pair<int, int>(es->virtual_process->vp_id, es->th_id)]];
        task = (detail::my_op_t *)parsec_thread_mempool_allocate(mempool);
        memset((void *)task, 0, sizeof(detail::my_op_t));
        task->parsec_task.mempool_owner = mempool;

        PARSEC_OBJ_CONSTRUCT(task, parsec_list_item_t);
        task->parsec_task.task_class = &this->self;
        task->parsec_task.taskpool = world_impl.taskpool();
        task->parsec_task.status = PARSEC_TASK_STATUS_HOOK;

        task->function_template_class_ptr[static_cast<std::size_t>(ttg::ExecutionSpace::Host)] =
            reinterpret_cast<detail::parsec_static_op_t>(&Op::static_op_noarg<ttg::ExecutionSpace::Host>);
        if constexpr (derived_has_cuda_op())
          task->function_template_class_ptr[static_cast<std::size_t>(ttg::ExecutionSpace::CUDA)] =
              reinterpret_cast<detail::parsec_static_op_t>(&Op::static_op_noarg<ttg::ExecutionSpace::CUDA>);
        task->object_ptr = static_cast<derivedT *>(this);
        keyT *kp = new keyT(key);
        task->key = reinterpret_cast<parsec_key_t>(kp);
        task->parsec_task.data[0].data_in = static_cast<ttg_data_copy_t *>(NULL);
        if (tracing()) ttg::print(world.rank(), ":", get_name(), " : ", key, ": creating task");
        world_impl.increment_created();
        if (tracing()) ttg::print(world.rank(), ":", get_name(), " : ", key, ": submitting task for op ");
        world_impl.increment_sent_to_sched();
        __parsec_schedule(es, &task->parsec_task, 0);
      } else {
        using msg_t = detail::msg_t;
        // We pass -1 to signal that we just need to call set_arg(key) on the other end
        std::unique_ptr<msg_t> msg =
            std::make_unique<msg_t>(get_instance_id(), world_impl.taskpool()->taskpool_id, -1, 1);

        uint64_t pos = 0;
        pos = pack(key, msg->bytes, pos);
        parsec_taskpool_t *tp = world_impl.taskpool();
        tp->tdm.module->outgoing_message_start(tp, owner, NULL);
        tp->tdm.module->outgoing_message_pack(tp, owner, NULL, NULL, 0);
        parsec_ce.send_am(&parsec_ce, world_impl.parsec_ttg_tag(), owner, static_cast<void *>(msg.get()),
                          sizeof(msg_header_t) + pos);
      }
    }

    // case 6
    template <typename Key = keyT>
    std::enable_if_t<ttg::meta::is_void_v<Key>, void> set_arg() {
      static_assert(ttg::meta::is_empty_tuple_v<input_refs_tuple_type>,
                    "logic error: set_arg (case 3) called but input_refs_tuple_type is nonempty");

      const auto owner = keymap();
      if (owner == ttg_default_execution_context().rank()) {
        // create PaRSEC task
        // and give it to the scheduler
        detail::my_op_t *task;
        auto &world_impl = world.impl();
        parsec_execution_stream_s *es = world_impl.execution_stream();
        parsec_thread_mempool_t *mempool =
            &mempools.thread_mempools[mempools_index[std::pair<int, int>(es->virtual_process->vp_id, es->th_id)]];
        task = (detail::my_op_t *)parsec_thread_mempool_allocate(mempool);
        memset((void *)task, 0, sizeof(detail::my_op_t));
        task->parsec_task.mempool_owner = mempool;

        PARSEC_OBJ_CONSTRUCT(task, parsec_list_item_t);
        task->parsec_task.task_class = &this->self;
        task->parsec_task.taskpool = world_impl.taskpool();
        task->parsec_task.status = PARSEC_TASK_STATUS_HOOK;

        task->function_template_class_ptr[static_cast<std::size_t>(ttg::ExecutionSpace::Host)] =
            reinterpret_cast<detail::parsec_static_op_t>(&Op::static_op_noarg<ttg::ExecutionSpace::Host>);
        if constexpr (derived_has_cuda_op())
          task->function_template_class_ptr[static_cast<std::size_t>(ttg::ExecutionSpace::CUDA)] =
              reinterpret_cast<detail::parsec_static_op_t>(&Op::static_op_noarg<ttg::ExecutionSpace::CUDA>);
        task->object_ptr = static_cast<derivedT *>(this);
        task->key = 0;
        task->parsec_task.data[0].data_in = static_cast<ttg_data_copy_t *>(NULL);
        if (tracing()) ttg::print(world.rank(), ":", get_name(), " : creating task");
        world_impl.increment_created();
        if (tracing()) ttg::print(world.rank(), ":", get_name(), " : submitting task for op ");
        world_impl.increment_sent_to_sched();
        __parsec_schedule(es, &task->parsec_task, 0);
      }
    }

    template <std::size_t i, typename Key, typename Value>
    std::enable_if_t<!ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>> &&
                         !ttg::has_split_metadata<std::decay_t<Value>>::value,
                     void>
    broadcast_arg(const ttg::span<const Key> &keylist, const Value &value) {
      using valueT = typename std::tuple_element<i, input_values_full_tuple_type>::type;
      auto world = ttg_default_execution_context();
      int rank = world.rank();

      bool have_remote = keylist.end() != std::find_if(keylist.begin(), keylist.end(),
                                                       [&](const Key &key) { return keymap(key) != rank; });

      if (have_remote) {
        std::vector<Key> keylist_sorted(keylist.begin(), keylist.end());

        /* Assuming there are no local keys, will be updated while processing remote keys */
        auto local_begin = keylist_sorted.end();
        auto local_end = keylist_sorted.end();

        /* sort the input key list by owner and check whether there are remote keys */
        std::sort(keylist_sorted.begin(), keylist_sorted.end(), [&](const Key &a, const Key &b) mutable {
          int rank_a = keymap(a);
          int rank_b = keymap(b);
          return rank_a < rank_b;
        });

        using msg_t = detail::msg_t;
        local_begin = keylist_sorted.end();
        auto &world_impl = world.impl();
        std::unique_ptr<msg_t> msg = std::make_unique<msg_t>(get_instance_id(), world_impl.taskpool()->taskpool_id, i);

        parsec_taskpool_t *tp = world_impl.taskpool();

        for (auto it = keylist_sorted.begin(); it < keylist_sorted.end(); /* increment inline */) {
          auto owner = keymap(*it);
          if (owner == rank) {
            /* make sure we don't lose local keys */
            local_begin = it;
            local_end =
                std::find_if_not(++it, keylist_sorted.end(), [&](const Key &key) { return keymap(key) == rank; });
            it = local_end;
            continue;
          }

          /* pack all keys for this owner */
          int num_keys = 0;
          uint64_t pos = 0;
          do {
            ++num_keys;
            pos = pack(*it, msg->bytes, pos);
            ++it;
          } while (it < keylist_sorted.end() && keymap(*it) == owner);
          msg->op_id.num_keys = num_keys;

          /* TODO: use RMA to transfer the value */
          pos = pack(value, msg->bytes, pos);

          /* Send the message */
          tp->tdm.module->outgoing_message_start(tp, owner, NULL);
          tp->tdm.module->outgoing_message_pack(tp, owner, NULL, NULL, 0);
          parsec_ce.send_am(&parsec_ce, world_impl.parsec_ttg_tag(), owner, static_cast<void *>(msg.get()),
                            sizeof(msg_header_t) + pos);
        }
        /* handle local keys */
        /* TODO: get the lifetime managment of the object straight! */
        if (std::distance(local_begin, local_end) > 0) {
          for (auto it = local_begin; it != local_end; ++it) {
            set_arg_local<i, keyT, Value>(*it, value);
          }
        }
      } else {
        /* only local keys */
        for (auto &key : keylist) {
          set_arg_local<i, keyT, Value>(key, value);
        }
      }
    }

    template <std::size_t i, typename Key, typename Value>
    std::enable_if_t<!ttg::meta::is_void_v<Key> && !std::is_void_v<std::decay_t<Value>> &&
                         ttg::has_split_metadata<std::decay_t<Value>>::value,
                     void>
    splitmd_broadcast_arg(const ttg::span<const Key> &keylist, const Value &value) {
      using valueT = typename std::tuple_element<i, input_values_full_tuple_type>::type;
      auto world = ttg_default_execution_context();
      int rank = world.rank();
      bool have_remote = keylist.end() != std::find_if(keylist.begin(), keylist.end(),
                                                       [&](const Key &key) { return keymap(key) != rank; });

      if (have_remote) {
        using decvalueT = std::decay_t<Value>;

        /* sort the input key list by owner and check whether there are remote keys */
        std::vector<Key> keylist_sorted(keylist.begin(), keylist.end());
        std::sort(keylist_sorted.begin(), keylist_sorted.end(), [&](const Key &a, const Key &b) mutable {
          int rank_a = keymap(a);
          int rank_b = keymap(b);
          return rank_a < rank_b;
        });

        /* Assuming there are no local keys, will be updated while iterating over the keys */
        auto local_begin = keylist_sorted.end();
        auto local_end = keylist_sorted.end();

        ttg::SplitMetadataDescriptor<decvalueT> descr;
        auto iovs = descr.get_data(*const_cast<decvalueT *>(&value));
        int32_t num_iovs = std::distance(std::begin(iovs), std::end(iovs));
        std::vector<std::pair<int32_t, std::shared_ptr<void>>> memregs;
        memregs.reserve(num_iovs);

        /**
         * NOTE: the high-level ttg::broadcast will create an object on the heap and pass us a
         *       shared_ptr that we capture and release one the RMA in the AM is complete.
         *       Thus, there is no need to track the task's copy here, that is only necessary for
         *       local keys.
         */

        /* register all iovs so the registration can be reused */
        for (auto iov : iovs) {
          parsec_ce_mem_reg_handle_t lreg;
          size_t lreg_size;
          parsec_ce.mem_register(iov.data, PARSEC_MEM_TYPE_NONCONTIGUOUS, iov.num_bytes, parsec_datatype_int8_t,
                                 iov.num_bytes, &lreg, &lreg_size);
          /* TODO: use a static function for deregistration here? */
          memregs.push_back(std::make_pair(static_cast<int32_t>(lreg_size),
                                           /* TODO: this assumes that parsec_ce_mem_reg_handle_t is void* */
                                           std::shared_ptr<void>{lreg, [](void *ptr) {
                                                                   parsec_ce_mem_reg_handle_t memreg =
                                                                       (parsec_ce_mem_reg_handle_t)ptr;
                                                                   parsec_ce.mem_unregister(&memreg);
                                                                 }}));
        }

        using msg_t = detail::msg_t;
        auto &world_impl = world.impl();
        std::unique_ptr<msg_t> msg = std::make_unique<msg_t>(get_instance_id(), world_impl.taskpool()->taskpool_id, i);
        auto metadata = descr.get_metadata(value);
        size_t metadata_size = sizeof(metadata);

        ttg_data_copy_t *copy;
        copy = detail::find_copy_in_task(parsec_ttg_caller, &value);
        assert(nullptr != copy);

        parsec_taskpool_t *tp = world_impl.taskpool();
        for (auto it = keylist_sorted.begin(); it < keylist_sorted.end(); /* increment done inline */) {
          auto owner = keymap(*it);
          if (owner == rank) {
            local_begin = it;
            /* find first non-local key */
            local_end =
                std::find_if_not(++it, keylist_sorted.end(), [&](const Key &key) { return keymap(key) == rank; });
            it = local_end;
            continue;
          }

          /* count keys and set it afterwards */
          uint64_t pos = 0;
          /* pack all keys for this owner */
          int num_keys = 0;
          do {
            ++num_keys;
            pos = pack(*it, msg->bytes, pos);
            ++it;
          } while (it < keylist_sorted.end() && keymap(*it) == owner);
          msg->op_id.num_keys = num_keys;

          /* pack the metadata */
          std::memcpy(msg->bytes + pos, &metadata, metadata_size);
          pos += metadata_size;
          /* pack the local rank */
          int rank = world.rank();
          std::memcpy(msg->bytes + pos, &rank, sizeof(rank));
          pos += sizeof(rank);
          /* pack the number of iovecs */
          std::memcpy(msg->bytes + pos, &num_iovs, sizeof(num_iovs));
          pos += sizeof(num_iovs);

          /* TODO: at the moment, the tag argument to parsec_ce.get() is treated as a
           * raw function pointer instead of a preregistered AM tag, so play that game.
           * Once this is fixed in PaRSEC we need to use parsec_ttg_rma_tag instead! */
          parsec_ce_tag_t cbtag = reinterpret_cast<parsec_ce_tag_t>(&detail::get_remote_complete_cb);
          std::memcpy(msg->bytes + pos, &cbtag, sizeof(cbtag));
          pos += sizeof(cbtag);

          /**
           * pack the registration handles
           * memory layout: [<lreg_size, lreg, lreg_fn>, ...]
           */
          int idx = 0;
          for (auto iov : iovs) {
            // auto [lreg_size, lreg_ptr] = memregs[idx];
            int32_t lreg_size;
            std::shared_ptr<void> lreg_ptr;
            std::tie(lreg_size, lreg_ptr) = memregs[idx];
            std::memcpy(msg->bytes + pos, &lreg_size, sizeof(lreg_size));
            pos += sizeof(lreg_size);
            std::memcpy(msg->bytes + pos, lreg_ptr.get(), lreg_size);
            pos += lreg_size;
            /* create a function that will be invoked upon RMA completion at the target */
            std::shared_ptr<void> lreg_ptr_v = lreg_ptr;
            /* mark another reader on the copy */
            copy = detail::register_data_copy<valueT>(copy, nullptr, true);
            std::function<void(void)> *fn = new std::function<void(void)>([=]() mutable {
              /* shared_ptr of value and registration captured by value so resetting
               * them here will eventually release the memory/registration */
              detail::release_data_copy(copy);
              lreg_ptr_v.reset();
            });
            std::intptr_t fn_ptr{reinterpret_cast<std::intptr_t>(fn)};
            std::memcpy(msg->bytes + pos, &fn_ptr, sizeof(fn_ptr));
            pos += sizeof(fn_ptr);
            ++idx;
          }
          tp->tdm.module->outgoing_message_start(tp, owner, NULL);
          tp->tdm.module->outgoing_message_pack(tp, owner, NULL, NULL, 0);
          parsec_ce.send_am(&parsec_ce, world_impl.parsec_ttg_tag(), owner, static_cast<void *>(msg.get()),
                            sizeof(msg_header_t) + pos);
        }
        /* handle local keys */
        for (auto it = local_begin; it != local_end; ++it) {
          set_arg_local_impl<i>(*it, value);
        }
      } else {
        /* handle local keys */
        for (auto it = keylist.begin(); it != keylist.end(); ++it) {
          set_arg_local_impl<i>(*it, value);
        }
      }
    }

    // Used by invoke to set all arguments associated with a task
    template <typename Key, size_t... IS>
    std::enable_if_t<ttg::meta::is_none_void_v<Key>, void> set_args(std::index_sequence<IS...>, const Key &key,
                                                                    const input_refs_tuple_type &args) {
      int junk[] = {0, (set_arg<IS>(key, Op::get<IS>(args)), 0)...};
      junk[0]++;
    }

    // Used by invoke to set all arguments associated with a task
    template <typename Key = keyT, size_t... IS>
    std::enable_if_t<ttg::meta::is_void_v<Key>, void> set_args(std::index_sequence<IS...>,
                                                               const input_refs_tuple_type &args) {
      int junk[] = {0, (set_arg<IS>(Op::get<IS>(args)), 0)...};
      junk[0]++;
    }

   public:
    /// sets stream size for input \c i
    /// \param size positive integer that specifies the stream size
    template <std::size_t i, typename Key>
    std::enable_if_t<!ttg::meta::is_void_v<Key>, void> set_argstream_size(const Key &key, std::size_t size) {
      // preconditions
      assert(std::get<i>(input_reducers) && "Op::set_argstream_size called on nonstreaming input terminal");
      assert(size > 0 && "Op::set_argstream_size(key,size) called with size=0");

      // body
      const auto owner = keymap(key);
      if (owner != world.rank()) {
        if (tracing()) {
          ttg::print(world.rank(), ":", get_name(), ":", key, " : forwarding stream size for terminal ", i);
        }
        // TODO: manage distributed case for streaming terminal
        //    worldobjT::send(owner, &opT::template set_argstream_size<i, true>, size);
        abort();
      } else {
        if (tracing()) {
          ttg::print(world.rank(), ":", get_name(), ":", key, " : setting stream size to ", size, " for terminal ", i);
        }

        auto hk = reinterpret_cast<parsec_key_t>(&key);
        detail::my_op_t *task = nullptr;
        parsec_hash_table_lock_bucket(&tasks_table, hk);
        if (nullptr == (task = (detail::my_op_t *)parsec_hash_table_nolock_find(&tasks_table, hk))) {
          task = newtask_with_lock(key);
        }

        // TODO: Unfriendly implementation, cannot check if stream is already bounded
        // TODO: Unfriendly implementation, cannot check if stream has been finalized already

        // commit changes
        task->stream_size[i] += (int)size;
        bool release = (task->stream_size[i] == 0);
        parsec_hash_table_unlock_bucket(&tasks_table, hk);

        if (release) release_task(this, task);
      }
    }

    /// sets stream size for input \c i
    /// \param size positive integer that specifies the stream size
    template <std::size_t i, typename Key = keyT>
    std::enable_if_t<ttg::meta::is_void_v<Key>, void> set_argstream_size(std::size_t size) {
      // preconditions
      assert(std::get<i>(input_reducers) && "Op::set_argstream_size called on nonstreaming input terminal");
      assert(size > 0 && "Op::set_argstream_size(key,size) called with size=0");

      // body
      const auto owner = keymap();
      if (owner != world.rank()) {
        if (tracing()) {
          ttg::print(world.rank(), ":", get_name(), " : forwarding stream size for terminal ", i);
        }
        // TODO: manage distributed case for streaming terminal
        //    worldobjT::send(owner, &opT::template set_argstream_size<i, true>, size);
        abort();
      } else {
        if (tracing()) {
          ttg::print(world.rank(), ":", get_name(), " : setting stream size to ", size, " for terminal ", i);
        }

        parsec_key_t hk = 0;
        detail::my_op_t *task = nullptr;
        parsec_hash_table_lock_bucket(&tasks_table, hk);
        if (nullptr == (task = (detail::my_op_t *)parsec_hash_table_nolock_find(&tasks_table, hk))) {
          task = newtask_with_lock(ttg::Void{});
        }

        // TODO: Unfriendly implementation, cannot check if stream is already bounded
        // TODO: Unfriendly implementation, cannot check if stream has been finalized already

        // commit changes
        task->stream_size[i] += (int)size;
        bool release = (task->stream_size[i] == 0);
        parsec_hash_table_unlock_bucket(&tasks_table, hk);

        if (release) release_task(this, task);
      }
    }

    /// finalizes stream for input \c i
    template <std::size_t i, typename Key>
    std::enable_if_t<!ttg::meta::is_void_v<Key>, void> finalize_argstream(const Key &key) {
      // preconditions
      assert(std::get<i>(input_reducers) && "Op::finalize_argstream called on nonstreaming input terminal");

      // body
      const auto owner = keymap(key);
      if (owner != world.rank()) {
        if (tracing()) {
          ttg::print(world.rank(), ":", get_name(), " : ", key, ": forwarding stream finalize for terminal ", i);
        }
        abort();  // TODO: implement distributed streaming terminals
      } else {
        if (tracing()) {
          ttg::print(world.rank(), ":", get_name(), " : ", key, ": finalizing stream for terminal ", i);
        }

        auto hk = reinterpret_cast<parsec_key_t>(&key);
        detail::my_op_t *task = nullptr;
        parsec_hash_table_lock_bucket(&tasks_table, hk);
        if (nullptr == (task = (detail::my_op_t *)parsec_hash_table_nolock_find(&tasks_table, hk))) {
          ttg::print_error(world.rank(), ":", get_name(), ":", key,
                           " : error finalize called on stream that never received an input data: ", i);
          throw std::runtime_error("Op::finalize called on stream that never received an input data");
        }

        // TODO: Unfriendly implementation, cannot check if stream is already bounded
        // TODO: Unfriendly implementation, cannot check if stream has been finalized already

        // commit changes
        task->stream_size[i] = 0;
        parsec_hash_table_unlock_bucket(&tasks_table, hk);

        release_task(this, task);
      }
    }

    /// finalizes stream for input \c i
    template <std::size_t i, bool key_is_void = ttg::meta::is_void_v<keyT>>
    std::enable_if_t<key_is_void, void> finalize_argstream() {
      // preconditions
      assert(std::get<i>(input_reducers) && "Op::finalize_argstream called on nonstreaming input terminal");

      // body
      const auto owner = keymap();
      if (owner != world.rank()) {
        if (tracing()) {
          ttg::print(world.rank(), ":", get_name(), ": forwarding stream finalize for terminal ", i);
        }
        abort();  // TODO: implement distributed streaming terminals
      } else {
        if (tracing()) {
          ttg::print(world.rank(), ":", get_name(), ": finalizing stream for terminal ", i);
        }

        auto hk = static_cast<parsec_key_t>(0);
        detail::my_op_t *task = nullptr;
        parsec_hash_table_lock_bucket(&tasks_table, hk);
        if (nullptr == (task = (detail::my_op_t *)parsec_hash_table_nolock_find(&tasks_table, hk))) {
          ttg::print_error(world.rank(), ":", get_name(),
                           " : error finalize called on stream that never received an input data: ", i);
          throw std::runtime_error("Op::finalize called on stream that never received an input data");
        }

        // TODO: Unfriendly implementation, cannot check if stream is already bounded
        // TODO: Unfriendly implementation, cannot check if stream has been finalized already

        // commit changes
        task->stream_size[i] = 0;
        parsec_hash_table_unlock_bucket(&tasks_table, hk);

        release_task(this, task);
      }
    }

   private:
    // Copy/assign/move forbidden ... we could make it work using
    // PIMPL for this base class.  However, this instance of the base
    // class is tied to a specific instance of a derived class a
    // pointer to which is captured for invoking derived class
    // functions.  Thus, not only does the derived class has to be
    // involved but we would have to do it in a thread safe way
    // including for possibly already running tasks and remote
    // references.  This is not worth the effort ... wherever you are
    // wanting to move/assign an Op you should be using a pointer.
    Op(const Op &other) = delete;
    Op &operator=(const Op &other) = delete;
    Op(Op &&other) = delete;
    Op &operator=(Op &&other) = delete;

    // Registers the callback for the i'th input terminal
    template <typename terminalT, std::size_t i>
    void register_input_callback(terminalT &input) {
      using valueT = typename terminalT::value_type;
      //////////////////////////////////////////////////////////////////
      // case 1: nonvoid key, nonvoid value
      //////////////////////////////////////////////////////////////////
      if constexpr (!ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                    !std::is_void_v<valueT>) {
        auto move_callback = [this](const keyT &key, valueT &&value) {
          set_arg<i, keyT, valueT>(key, std::forward<valueT>(value));
        };
        auto send_callback = [this](const keyT &key, const valueT &value) {
          set_arg<i, keyT, const valueT &>(key, value);
        };
        auto broadcast_callback = [this](const ttg::span<const keyT> &keylist, const valueT &value) {
          if constexpr (ttg::has_split_metadata<std::decay_t<valueT>>::value) {
            splitmd_broadcast_arg<i, keyT, valueT>(keylist, value);
          } else {
            broadcast_arg<i, keyT, valueT>(keylist, value);
          }
        };
        auto setsize_callback = [this](const keyT &key, std::size_t size) { set_argstream_size<i>(key, size); };
        auto finalize_callback = [this](const keyT &key) { finalize_argstream<i>(key); };
        input.set_callback(send_callback, move_callback, broadcast_callback, setsize_callback, finalize_callback);
      }
      //////////////////////////////////////////////////////////////////
      // case 2: nonvoid key, void value, mixed inputs
      //////////////////////////////////////////////////////////////////
      else if constexpr (!ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                         std::is_void_v<valueT>) {
        auto send_callback = [this](const keyT &key) { set_arg<i, keyT, ttg::Void>(key, ttg::Void{}); };
        auto setsize_callback = [this](const keyT &key, std::size_t size) { set_argstream_size<i>(key, size); };
        auto finalize_callback = [this](const keyT &key) { finalize_argstream<i>(key); };
        input.set_callback(send_callback, send_callback, {}, setsize_callback, finalize_callback);
      }
      //////////////////////////////////////////////////////////////////
      // case 3: nonvoid key, void value, no inputs
      //////////////////////////////////////////////////////////////////
      else if constexpr (!ttg::meta::is_void_v<keyT> && ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                         std::is_void_v<valueT>) {
        auto send_callback = [this](const keyT &key) { set_arg<keyT>(key); };
        auto setsize_callback = [this](const keyT &key, std::size_t size) { set_argstream_size<i>(key, size); };
        auto finalize_callback = [this](const keyT &key) { finalize_argstream<i>(key); };
        input.set_callback(send_callback, send_callback, {}, setsize_callback, finalize_callback);
      }
      //////////////////////////////////////////////////////////////////
      // case 4: void key, nonvoid value
      //////////////////////////////////////////////////////////////////
      else if constexpr (ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                         !std::is_void_v<valueT>) {
        auto move_callback = [this](valueT &&value) { set_arg<i, keyT, valueT>(std::forward<valueT>(value)); };
        auto send_callback = [this](const valueT &value) { set_arg<i, keyT, const valueT &>(value); };
        auto setsize_callback = [this](std::size_t size) { set_argstream_size<i>(size); };
        auto finalize_callback = [this]() { finalize_argstream<i>(); };
        input.set_callback(send_callback, move_callback, {}, setsize_callback, finalize_callback);
      }
      //////////////////////////////////////////////////////////////////
      // case 5: void key, void value, mixed inputs
      //////////////////////////////////////////////////////////////////
      else if constexpr (ttg::meta::is_void_v<keyT> && !ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                         std::is_void_v<valueT>) {
        auto send_callback = [this]() { set_arg<i, keyT, ttg::Void>(ttg::Void{}); };
        auto setsize_callback = [this](std::size_t size) { set_argstream_size<i>(size); };
        auto finalize_callback = [this]() { finalize_argstream<i>(); };
        input.set_callback(send_callback, send_callback, {}, setsize_callback, finalize_callback);
      }
      //////////////////////////////////////////////////////////////////
      // case 6: void key, void value, no inputs
      //////////////////////////////////////////////////////////////////
      else if constexpr (ttg::meta::is_void_v<keyT> && ttg::meta::is_empty_tuple_v<input_refs_tuple_type> &&
                         std::is_void_v<valueT>) {
        auto send_callback = [this]() { set_arg<keyT>(); };
        auto setsize_callback = [this](std::size_t size) { set_argstream_size<i>(size); };
        auto finalize_callback = [this]() { finalize_argstream<i>(); };
        input.set_callback(send_callback, send_callback, {}, setsize_callback, finalize_callback);
      } else
        abort();
    }

    template <std::size_t... IS>
    void register_input_callbacks(std::index_sequence<IS...>) {
      int junk[] = {0, (register_input_callback<typename std::tuple_element<IS, input_terminals_type>::type, IS>(
                            std::get<IS>(input_terminals)),
                        0)...};
      junk[0]++;
    }

    template <std::size_t... IS, typename inedgesT>
    void connect_my_inputs_to_incoming_edge_outputs(std::index_sequence<IS...>, inedgesT &inedges) {
      int junk[] = {0, (std::get<IS>(inedges).set_out(&std::get<IS>(input_terminals)), 0)...};
      junk[0]++;
    }

    template <std::size_t... IS, typename outedgesT>
    void connect_my_outputs_to_outgoing_edge_inputs(std::index_sequence<IS...>, outedgesT &outedges) {
      int junk[] = {0, (std::get<IS>(outedges).set_in(&std::get<IS>(output_terminals)), 0)...};
      junk[0]++;
    }

    template <typename input_terminals_tupleT, std::size_t... IS, typename flowsT>
    void _initialize_flows(std::index_sequence<IS...>, flowsT &&flows) {
      int junk[] = {0,
                    (*(const_cast<std::remove_const_t<decltype(flows[IS]->flow_flags)> *>(&(flows[IS]->flow_flags))) =
                         (std::is_const<typename std::tuple_element<IS, input_terminals_tupleT>::type>::value
                              ? PARSEC_FLOW_ACCESS_READ
                              : PARSEC_FLOW_ACCESS_RW),
                     0)...};
      junk[0]++;
    }

    template <typename input_terminals_tupleT, typename flowsT>
    void initialize_flows(flowsT &&flows) {
      _initialize_flows<input_terminals_tupleT>(
          std::make_index_sequence<std::tuple_size<input_terminals_tupleT>::value>{}, flows);
    }

    void fence() override { ttg::get_default_world().impl().fence(); }

    static int key_equal(parsec_key_t a, parsec_key_t b, void *user_data) {
      if constexpr (std::is_same_v<keyT, void>) {
        return 1;
      } else {
        keyT &ka = *(reinterpret_cast<keyT *>(a));
        keyT &kb = *(reinterpret_cast<keyT *>(b));
        return ka == kb;
      }
    }

    static uint64_t key_hash(parsec_key_t k, void *user_data) {
      if constexpr (std::is_same_v<keyT, void>) {
        return 0;
      } else {
        keyT &kk = *(reinterpret_cast<keyT *>(k));
        using ttg::hash;
        uint64_t hv = hash<decltype(kk)>{}(kk);
        return hv;
      }
    }

    static char *key_print(char *buffer, size_t buffer_size, parsec_key_t k, void *user_data) {
      if constexpr (std::is_same_v<keyT, void>) {
        buffer[0] = '\0';
        return buffer;
      } else {
        keyT kk = *(reinterpret_cast<keyT *>(k));
        std::stringstream iss;
        iss << kk;
        memset(buffer, 0, buffer_size);
        iss.get(buffer, buffer_size);
        return buffer;
      }
    }

    parsec_key_fn_t tasks_hash_fcts = {key_equal, key_print, key_hash};

    static parsec_hook_return_t complete_task_and_release(parsec_execution_stream_t *es, parsec_task_t *task) {
      for (int i = 0; i < MAX_PARAM_COUNT; i++) {
        ttg_data_copy_t *copy = reinterpret_cast<ttg_data_copy_t *>(task->data[i].data_in);
        detail::release_data_copy(copy);
        task->data[i].data_in = nullptr;
      }
      detail::my_op_t *op = (detail::my_op_t *)task;
      if (op->deferred_release) {
        op->deferred_release = nullptr;
        op->op_ptr = nullptr;
      }
      if constexpr (!ttg::meta::is_void_v<keyT>) {
        keyT *key = (keyT *)op->key;
        delete (key);
      }
      return PARSEC_HOOK_RETURN_DONE;
    }

   public:
    template <typename keymapT = ttg::detail::default_keymap<keyT>,
              typename priomapT = ttg::detail::default_priomap<keyT>>
    Op(const std::string &name, const std::vector<std::string> &innames, const std::vector<std::string> &outnames,
       ttg::World world, keymapT &&keymap_ = keymapT(), priomapT &&priomap_ = priomapT())
        : ttg::OpBase(name, numins, numouts)
        , set_arg_from_msg_fcts(make_set_args_fcts(std::make_index_sequence<numins>{}))
        , world(world)
        // if using default keymap, rebind to the given world
        , keymap(std::is_same<keymapT, ttg::detail::default_keymap<keyT>>::value
                     ? decltype(keymap)(ttg::detail::default_keymap<keyT>(world))
                     : decltype(keymap)(std::forward<keymapT>(keymap_)))
        , priomap(decltype(keymap)(std::forward<priomapT>(priomap_))) {
      // Cannot call these in base constructor since terminals not yet constructed
      if (innames.size() != std::tuple_size<input_terminals_type>::value)
        throw std::logic_error("ttg_parsec::OP: #input names != #input terminals");
      if (outnames.size() != std::tuple_size<output_terminalsT>::value)
        throw std::logic_error("ttg_parsec::OP: #output names != #output terminals");

      auto &world_impl = world.impl();
      world_impl.register_op(this);

      register_input_terminals(input_terminals, innames);
      register_output_terminals(output_terminals, outnames);

      register_input_callbacks(std::make_index_sequence<numins>{});

      int i;

      memset(&self, 0, sizeof(parsec_task_class_t));

      self.name = get_name().c_str();
      self.task_class_id = get_instance_id();
      self.nb_parameters = 0;
      self.nb_locals = 0;
      self.nb_flows = std::max((int)numins, (int)numouts);

      //    function_id_to_instance[self.task_class_id] = this;

      if constexpr (derived_has_cuda_op()) {
        self.incarnations = (__parsec_chore_t *)malloc(3 * sizeof(__parsec_chore_t));
        ((__parsec_chore_t *)self.incarnations)[0].type = PARSEC_DEV_CUDA;
        ((__parsec_chore_t *)self.incarnations)[0].evaluate = NULL;
        ((__parsec_chore_t *)self.incarnations)[0].hook = detail::hook_cuda;
        ((__parsec_chore_t *)self.incarnations)[1].type = PARSEC_DEV_CPU;
        ((__parsec_chore_t *)self.incarnations)[1].evaluate = NULL;
        ((__parsec_chore_t *)self.incarnations)[1].hook = detail::hook;
        ((__parsec_chore_t *)self.incarnations)[2].type = PARSEC_DEV_NONE;
        ((__parsec_chore_t *)self.incarnations)[2].evaluate = NULL;
        ((__parsec_chore_t *)self.incarnations)[2].hook = NULL;
      } else {
        self.incarnations = (__parsec_chore_t *)malloc(2 * sizeof(__parsec_chore_t));
        ((__parsec_chore_t *)self.incarnations)[0].type = PARSEC_DEV_CPU;
        ((__parsec_chore_t *)self.incarnations)[0].evaluate = NULL;
        ((__parsec_chore_t *)self.incarnations)[0].hook = detail::hook;
        ((__parsec_chore_t *)self.incarnations)[1].type = PARSEC_DEV_NONE;
        ((__parsec_chore_t *)self.incarnations)[1].evaluate = NULL;
        ((__parsec_chore_t *)self.incarnations)[1].hook = NULL;
      }

      self.release_task = parsec_release_task_to_mempool_update_nbtasks;
      self.complete_execution = complete_task_and_release;

      for (i = 0; i < numins; i++) {
        parsec_flow_t *flow = new parsec_flow_t;
        flow->name = strdup((std::string("flow in") + std::to_string(i)).c_str());
        flow->sym_type = PARSEC_SYM_INOUT;
        // see initialize_flows below
        // flow->flow_flags = PARSEC_FLOW_ACCESS_RW;
        flow->dep_in[0] = NULL;
        flow->dep_out[0] = NULL;
        flow->flow_index = i;
        flow->flow_datatype_mask = (1 << i);
        *((parsec_flow_t **)&(self.in[i])) = flow;
      }
      *((parsec_flow_t **)&(self.in[i])) = NULL;
      initialize_flows<input_terminals_type>(self.in);

      for (i = 0; i < numouts; i++) {
        parsec_flow_t *flow = new parsec_flow_t;
        flow->name = strdup((std::string("flow out") + std::to_string(i)).c_str());
        flow->sym_type = PARSEC_SYM_INOUT;
        flow->flow_flags = PARSEC_FLOW_ACCESS_READ;  // does PaRSEC use this???
        flow->dep_in[0] = NULL;
        flow->dep_out[0] = NULL;
        flow->flow_index = i;
        flow->flow_datatype_mask = (1 << i);
        *((parsec_flow_t **)&(self.out[i])) = flow;
      }
      *((parsec_flow_t **)&(self.out[i])) = NULL;

      self.flags = 0;
      self.dependencies_goal = numins; /* (~(uint32_t)0) >> (32 - numins); */

      int k = 0;
      auto *context = world_impl.context();
      for (int i = 0; i < context->nb_vp; i++) {
        for (int j = 0; j < context->virtual_processes[i]->nb_cores; j++) {
          mempools_index[std::pair<int, int>(i, j)] = k++;
        }
      }
      parsec_mempool_construct(&mempools, PARSEC_OBJ_CLASS(parsec_task_t), sizeof(detail::my_op_t),
                               offsetof(parsec_task_t, mempool_owner), k);

      parsec_hash_table_init(&tasks_table, offsetof(detail::my_op_t, op_ht_item), 8, tasks_hash_fcts, NULL);
    }

    template <typename keymapT = ttg::detail::default_keymap<keyT>,
              typename priomapT = ttg::detail::default_priomap<keyT>>
    Op(const std::string &name, const std::vector<std::string> &innames, const std::vector<std::string> &outnames,
       keymapT &&keymap = keymapT(ttg::get_default_world()), priomapT &&priomap = priomapT())
        : Op(name, innames, outnames, ttg::get_default_world(), std::forward<keymapT>(keymap),
             std::forward<priomapT>(priomap)) {}

    template <typename keymapT = ttg::detail::default_keymap<keyT>,
              typename priomapT = ttg::detail::default_priomap<keyT>>
    Op(const input_edges_type &inedges, const output_edges_type &outedges, const std::string &name,
       const std::vector<std::string> &innames, const std::vector<std::string> &outnames, ttg::World world,
       keymapT &&keymap_ = keymapT(), priomapT &&priomap = priomapT())
        : Op(name, innames, outnames, world, std::forward<keymapT>(keymap_), std::forward<priomapT>(priomap)) {
      connect_my_inputs_to_incoming_edge_outputs(std::make_index_sequence<numins>{}, inedges);
      connect_my_outputs_to_outgoing_edge_inputs(std::make_index_sequence<numouts>{}, outedges);
    }
    template <typename keymapT = ttg::detail::default_keymap<keyT>,
              typename priomapT = ttg::detail::default_priomap<keyT>>
    Op(const input_edges_type &inedges, const output_edges_type &outedges, const std::string &name,
       const std::vector<std::string> &innames, const std::vector<std::string> &outnames,
       keymapT &&keymap = keymapT(ttg::get_default_world()), priomapT &&priomap = priomapT())
        : Op(inedges, outedges, name, innames, outnames, ttg::get_default_world(), std::forward<keymapT>(keymap),
             std::forward<priomapT>(priomap)) {}

    // Destructor checks for unexecuted tasks
    ~Op() { release(); }

    static void ht_iter_cb(void *item, void *cb_data) {
      detail::my_op_t *task = (detail::my_op_t *)item;
      opT *op = (opT *)cb_data;
      if constexpr (!std::is_void_v<keyT>) {
        std::cout << "Left over task " << op->get_name() << " " << *(keyT *)task->key << std::endl;
      } else {
        std::cout << "Left over task " << op->get_name() << " " << task->key << std::endl;
      }
    }

    virtual void release() override {
      if (!alive) {
        return;
      }
      alive = false;
      /* print all outstanding tasks */
      parsec_hash_table_for_all(&tasks_table, ht_iter_cb, this);
      parsec_hash_table_fini(&tasks_table);
      parsec_mempool_destruct(&mempools);
      // uintptr_t addr = (uintptr_t)self.incarnations;
      // free((void *)addr);
      free((__parsec_chore_t *)self.incarnations);
      for (int i = 0; i < self.nb_flows; i++) {
        if (NULL != self.in[i]) {
          free(self.in[i]->name);
          delete self.in[i];
        }
        if (NULL != self.out[i]) {
          free(self.out[i]->name);
          delete self.out[i];
        }
      }
      world.impl().deregister_op(this);
    }

    static constexpr const ttg::Runtime runtime = ttg::Runtime::PaRSEC;

    template <std::size_t i, typename Reducer>
    void set_input_reducer(Reducer &&reducer) {
      std::get<i>(input_reducers) = reducer;
    }

    // Returns reference to input terminal i to facilitate connection --- terminal
    // cannot be copied, moved or assigned
    template <std::size_t i>
    typename std::tuple_element<i, input_terminals_type>::type *in() {
      return &std::get<i>(input_terminals);
    }

    // Returns reference to output terminal for purpose of connection --- terminal
    // cannot be copied, moved or assigned
    template <std::size_t i>
    typename std::tuple_element<i, output_terminalsT>::type *out() {
      return &std::get<i>(output_terminals);
    }

    // Manual injection of a task with all input arguments specified as a tuple
    template <typename Key = keyT>
    std::enable_if_t<!ttg::meta::is_void_v<Key>, void> invoke(const Key &key, const input_refs_tuple_type &args) {
      TTG_OP_ASSERT_EXECUTABLE();
      set_args(std::make_index_sequence<std::tuple_size<input_refs_tuple_type>::value>{}, key, args);
    }

    // Manual injection of a key-free task and all input arguments specified as a tuple
    template <typename Key = keyT>
    std::enable_if_t<ttg::meta::is_void_v<Key>, void> invoke(const input_refs_tuple_type &args) {
      TTG_OP_ASSERT_EXECUTABLE();
      set_args(std::make_index_sequence<std::tuple_size<input_refs_tuple_type>::value>{}, args);
    }

    // Manual injection of a task that has no arguments
    template <typename Key = keyT>
    std::enable_if_t<!ttg::meta::is_void_v<Key>, void> invoke(const Key &key) {
      TTG_OP_ASSERT_EXECUTABLE();
      set_arg<keyT>(key);
    }

    // Manual injection of a task that has no key or arguments
    template <typename Key = keyT>
    std::enable_if_t<ttg::meta::is_void_v<Key>, void> invoke() {
      TTG_OP_ASSERT_EXECUTABLE();
      set_arg<keyT>();
    }

    void make_executable() override {
      register_static_op_function();
      OpBase::make_executable();
    }

    /// keymap accessor
    /// @return the keymap
    const decltype(keymap) &get_keymap() const { return keymap; }

    /// keymap setter
    template <typename Keymap>
    void set_keymap(Keymap &&km) {
      keymap = km;
    }

    /// priority map accessor
    /// @return the priority map
    const decltype(priomap) &get_priomap() const { return priomap; }

    /// priomap setter
    /// @arg pm a function that maps a key to an integral priority value.
    template <typename Priomap>
    void set_priomap(Priomap &&pm) {
      priomap = pm;
    }

    // Register the static_op function to associate it to instance_id
    void register_static_op_function(void) {
      int rank;
      MPI_Comm_rank(MPI_COMM_WORLD, &rank);
      if (tracing()) {
        ttg::print("ttg_parsec(", rank, ") Inserting into static_id_to_op_map at ", get_instance_id());
      }
      static_set_arg_fct_call_t call = std::make_pair(&Op::static_set_arg, this);
      auto &world_impl = world.impl();
      static_map_mutex.lock();
      static_id_to_op_map.insert(std::make_pair(get_instance_id(), call));
      if (delayed_unpack_actions.count(get_instance_id()) > 0) {
        auto tp = world_impl.taskpool();

        if (tracing()) {
          ttg::print("ttg_parsec(", rank, ") There are ", delayed_unpack_actions.count(get_instance_id()),
                     " messages delayed with op_id ", get_instance_id());
        }

        auto se = delayed_unpack_actions.equal_range(get_instance_id());
        std::vector<static_set_arg_fct_arg_t> tmp;
        for (auto it = se.first; it != se.second;) {
          assert(it->first == get_instance_id());
          tmp.push_back(it->second);
          it = delayed_unpack_actions.erase(it);
        }
        static_map_mutex.unlock();

        for (auto it : tmp) {
          if (tracing()) {
            ttg::print("ttg_parsec(", rank, ") Unpacking delayed message (", ", ", get_instance_id(), ", ",
                       std::get<1>(it), ", ", std::get<2>(it), ")");
          }
          int rc = detail::static_unpack_msg(&parsec_ce, world_impl.parsec_ttg_tag(), std::get<1>(it), std::get<2>(it),
                                             std::get<0>(it), NULL);
          assert(rc == 0);
          free(std::get<1>(it));
        }

        tmp.clear();
      } else {
        static_map_mutex.unlock();
      }
    }
  };

#include "ttg/wrap.h"

}  // namespace ttg_parsec

/**
 * The PaRSEC backend tracks data copies so we make a copy of the data
 * if the data is not being tracked yet or if the data is not const, i.e.,
 * the user may mutate the data after it was passed to send/broadcast.
 */
template <>
struct ttg::detail::value_copy_handler<ttg::Runtime::PaRSEC> {
 private:
  ttg_data_copy_t *copy_to_remove = nullptr;

 public:
  ~value_copy_handler() {
    if (nullptr != copy_to_remove) {
      ttg_parsec::detail::remove_data_copy(copy_to_remove, parsec_ttg_caller);
      ttg_parsec::detail::release_data_copy(copy_to_remove);
    }
  }

  template <typename Value>
  inline Value &&operator()(Value &&value) {
    if (nullptr == parsec_ttg_caller) {
      ttg::print("ERROR: ttg_send or ttg_broadcast called outside of a task!\n");
    }
    ttg_data_copy_t *copy;
    copy = ttg_parsec::detail::find_copy_in_task(parsec_ttg_caller, &value);
    Value *value_ptr = &value;
    if (nullptr == copy) {
      /**
       * the value is not known, create a copy that we can track
       * depending on Value, this uses either the copy or move constructor
       */
      copy = PARSEC_OBJ_NEW(ttg_data_copy_t);
      value_ptr = new Value(std::forward<Value>(value));
      copy->device_private = value_ptr;
      copy->readers = 1;
      copy->delete_fn = &ttg_parsec::detail::typed_delete_t<Value>::delete_type;
      bool inserted = ttg_parsec::detail::add_copy_to_task(copy, parsec_ttg_caller);
      assert(inserted);
      copy_to_remove = copy;
    }
    return std::move(*value_ptr);
  }

  template <typename Value>
  inline const Value &operator()(const Value &value) {
    if (nullptr == parsec_ttg_caller) {
      ttg::print("ERROR: ttg_send or ttg_broadcast called outside of a task!\n");
    }
    ttg_data_copy_t *copy;
    copy = ttg_parsec::detail::find_copy_in_task(parsec_ttg_caller, &value);
    const Value *value_ptr = &value;
    if (nullptr == copy) {
      /**
       * the value is not known, create a copy that we can track
       * depending on Value, this uses either the copy or move constructor
       */
      copy = PARSEC_OBJ_NEW(ttg_data_copy_t);
      value_ptr = new Value(value);
      copy->device_private = const_cast<Value *>(value_ptr);
      copy->readers = 1;
      copy->delete_fn = &ttg_parsec::detail::typed_delete_t<Value>::delete_type;
      bool inserted = ttg_parsec::detail::add_copy_to_task(copy, parsec_ttg_caller);
      assert(inserted);
      copy_to_remove = copy;
    }
    return *value_ptr;
  }

  /* we have to make a copy of non-const data as the user may modify it after
   * send/broadcast */
  template <typename Value, typename Enabler = std::enable_if_t<!std::is_const_v<Value>>>
  inline Value &operator()(Value &value) {
    if (nullptr == parsec_ttg_caller) {
      ttg::print("ERROR: ttg_send or ttg_broadcast called outside of a task!\n");
    }
    ttg_data_copy_t *copy;
    /* the value is not known, create a copy that we can track */
    copy = PARSEC_OBJ_NEW(ttg_data_copy_t);
    Value *value_ptr = new Value(value);
    copy->device_private = value_ptr;
    copy->readers = 1;
    copy->delete_fn = &ttg_parsec::detail::typed_delete_t<Value>::delete_type;
    bool inserted = ttg_parsec::detail::add_copy_to_task(copy, parsec_ttg_caller);
    assert(inserted);
    copy_to_remove = copy;
    return *value_ptr;
  }
};

#endif  // PARSEC_TTG_H_INCLUDED
