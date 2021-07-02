#pragma once

#include <stdint.h>
#include <vector>
#include <queue>
#include <memory>
#include <functional>
#include <algorithm>
#include <unordered_map>
#include <iostream>

using v_seq_no_t = uint64_t;
using space_id_t = uint32_t;;
using page_no_t = uint32_t;
using boffset_t = uint32_t;

#define DBUG_LEV 0
/*template<class ...Args>
  This is for above c++17
  void LOG_PRINT(Args && ...args) {
  if (DBUG_LEV)
  (std::cout << ... << args);
  }*/
void __LOG_PRINT(){};
template<class F, class ...R>
void __LOG_PRINT(F&& f, R&& ...r) {
  if (DBUG_LEV) {
    std::cout << std::forward<F>(f);
    __LOG_PRINT(std::forward<R>(r)...);
  }
}
template<class F, class ...R>
void LOG_PRINT(F&& f, R&& ...r) {
  if (DBUG_LEV) {
    __LOG_PRINT(std::forward<F>(f), std::forward<R>(r)...);
  }
}


class WorkQueue {
  friend class GCHelper;
  public:
  WorkQueue(){};
  ~WorkQueue(){};

  template<class F, class... Args>
    void enqueue(F&& f, Args&&... args);

  void dequeue();

  private:
  std::queue<std::function<void()> > tasks;
};

  template<class F, class... Args>
void WorkQueue::enqueue(F&& f, Args&&... args) 
{
  std::function<void()> task = 
    std::bind(std::forward<F>(f), std::forward<Args>(args)...);

  this->tasks.emplace([task]() { task(); });
}

void WorkQueue::dequeue() 
{
  if (this->tasks.empty()) 
    std::runtime_error("Exception: WorkQueue is empty, but function is called");

  std::function<void()> task;

  task = std::move(this->tasks.front());
  this->tasks.pop();

  task();
}

class MetaQueue {
  friend class GCHelper;
  private:
  // This is for unordered map only.
  struct __Key {
    __Key(space_id_t s, page_no_t p, boffset_t b) : s(s), p(p), b(b) {};
    space_id_t s;
    page_no_t p;
    boffset_t b;

    bool operator==(const __Key &other) const {
      return (s == other.s && p == other.p && b == other.b);
    }
  };

  struct __KeyHash {
    std::size_t operator()(const __Key& k) const {
      return ((std::hash<space_id_t>()(k.s) 
            ^ (std::hash<page_no_t>()(k.p) << 1)) >> 1)
        ^ (std::hash<boffset_t>()(k.b) << 1);
    }
  };

  public:
  struct __Info {
    __Info(v_seq_no_t v, space_id_t s, page_no_t p, boffset_t b) :
      view_seq_no(v), space_id(s), page_no(p), boffset(b){};
    v_seq_no_t view_seq_no;
    space_id_t space_id;
    page_no_t page_no;
    boffset_t boffset;
  };

  public:
  MetaQueue(){};
  ~MetaQueue(){};

  bool enqueue(v_seq_no_t view_seq_no, space_id_t space_id,
      page_no_t page_no, boffset_t boffset) {

    // Duplicate check
    __Key tmp_key(space_id, page_no, boffset);

    auto h_it = hash_map.find(tmp_key);
    if (h_it != hash_map.end()) {
      return false;
    } else {
      hash_map.insert(std::make_pair<__Key, bool>(std::move(tmp_key), true));
    }


    auto s_it = space_ref_cnt.find(space_id);

    // Prevent to truncate a log tablespaces
    if (s_it != space_ref_cnt.end()) {
      (s_it->second)++;
    } else {
      space_ref_cnt.insert(std::make_pair<space_id_t, uint32_t>(
            std::move(space_id), static_cast<uint32_t>(1)));
    }

    queue.emplace(__Info(view_seq_no, space_id, page_no, boffset));
    return true;
  }

  bool dequeue(v_seq_no_t min_seq_no) {

    //LOG_PRINT("meta-dequeue= SEQNO:", min_seq_no);
    if (queue.empty())
      return false;

    __Info tmp_info = std::move(queue.front());
    __Key tmp_key(tmp_info.space_id, tmp_info.page_no, tmp_info.boffset);
    LOG_PRINT("dequeue= R:", tmp_info.space_id, ", P:", tmp_info.page_no,
        ", B:", tmp_info.boffset, "\n");
    if (tmp_info.view_seq_no >= min_seq_no)
      return false;

    if (hash_map.erase(tmp_key) != 1) {
      // error
      throw std::runtime_error(
          "Exception: Fail to erase an element in hash map");
    }

    auto s_it = space_ref_cnt.find(tmp_info.space_id);

    // Prevent to truncate a log tablespaces
    if (s_it != space_ref_cnt.end()) {
      if ((s_it->second) == 1) {
        space_ref_cnt.erase(s_it);
      } else {
        (s_it->second)--;
      }
    } else {
      throw std::runtime_error(
          "Exception: Fail to find element in space ref hash map.");
    }

    queue.pop();
    return true;
  }

  bool __check_truncation(space_id_t s) {
    auto s_it = space_ref_cnt.find(s);

    return (s_it == space_ref_cnt.end());
  }

  private:
  std::queue<__Info> queue;
  std::unordered_map<__Key, bool, __KeyHash> hash_map;
  
  /** Prevent to truncate undo log tablesapce when the work related to 
    the tablespace is in work queue */
  std::unordered_map<space_id_t, uint32_t> space_ref_cnt;

};

class GCHelper {
  public:
    GCHelper() {
      this->w_queue = new WorkQueue();
      this->m_queue = new MetaQueue();
    }

    ~GCHelper() {
      delete this->w_queue;
      delete this->m_queue;
    }

    template<class F, class... Args>
      bool push_element(v_seq_no_t, space_id_t, page_no_t, boffset_t,
          F&& f, Args&&... args);

    void pop_elements(v_seq_no_t seq_no);

    bool check_truncation(space_id_t space_id);
  private:
    WorkQueue* w_queue;
    MetaQueue* m_queue;

};

template<class F, class... Args>
bool GCHelper::push_element(v_seq_no_t v, space_id_t s, page_no_t p, boffset_t b,
    F&& f, Args&&... args) {
  if (!this->m_queue->enqueue(v, s, p, b)){
    return false;
  }

  LOG_PRINT("push_element= V: ", v, ", S: ", s, ", P: ", p, ": B: ", b, '\n');

  this->w_queue->enqueue(f, args...);
  //LOG_PRINT("push_element= Success to enqueue\n");
  return true;
}

void GCHelper::pop_elements(v_seq_no_t seq_no) {
  //LOG_PRINT("pop_elements= SEQ_NO: ", seq_no);
  do {
    if (!m_queue->dequeue(seq_no))
      break;
    else
      w_queue->dequeue();

    //LOG_PRINT("pop_elements= Dequeue\n");
  } while(1);
  //LOG_PRINT("pop_elements= Done\n");
}

bool GCHelper::check_truncation(space_id_t space_id) {
  return m_queue->__check_truncation(space_id); 
}
