#ifndef CHARPORT_CHARVEC_STORE_H
#define CHARPORT_CHARVEC_STORE_H

// Native charvec payload: contiguous record arrays plus owned payload slices.

#include "detail.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>

namespace charport {
namespace charvec {
namespace components {

inline const char * empty_data() noexcept {
  static const char empty = '\0';
  return &empty;
}

inline charport_strview na_record() noexcept {
  return make_strview(nullptr, NA_INTEGER, CETYPE_EXT_NA);
}

inline charport_strview empty_record(cetype_ext_t enc) noexcept {
  return make_strview(empty_data(), 0, enc);
}

inline int checked_string_size(size_t len, const char * what = "string length") {
  if(!internal::check_r_string_len(len)) {
    throw std::runtime_error(std::string(what) + " exceeds R string size");
  }
  return static_cast<int>(len);
}

class SliceChain {
  struct Slice {
    Slice * next;
  };

  Slice * head_ = nullptr;

  static Slice * allocate(size_t payload_bytes, Slice * next) {
    if(payload_bytes > std::numeric_limits<size_t>::max() - sizeof(Slice)) {
      throw std::bad_alloc();
    }
    void * mem = ::operator new(sizeof(Slice) + payload_bytes);
    return new(mem) Slice{next};
  }

  static void deallocate(Slice * slice) noexcept {
    slice->~Slice();
    ::operator delete(slice);
  }

  static char * data(Slice * slice) noexcept {
    return reinterpret_cast<char *>(slice) + sizeof(Slice);
  }

public:
  SliceChain() noexcept = default;
  SliceChain(const SliceChain &) = delete;
  SliceChain & operator=(const SliceChain &) = delete;

  SliceChain(SliceChain && other) noexcept : head_(other.head_) {
    other.head_ = nullptr;
  }

  SliceChain & operator=(SliceChain && other) noexcept {
    if(this != &other) {
      clear();
      head_ = other.head_;
      other.head_ = nullptr;
    }
    return *this;
  }

  ~SliceChain() { clear(); }

  bool empty() const noexcept { return head_ == nullptr; }

  char * front_data() noexcept {
    return head_ == nullptr ? nullptr : data(head_);
  }

  char * push_front(size_t payload_bytes) {
    Slice * slice = allocate(payload_bytes, head_);
    head_ = slice;
    return data(slice);
  }

  char * data_slice(size_t idx) noexcept {
    Slice * slice = head_;
    while(slice != nullptr && idx > 0) {
      slice = slice->next;
      --idx;
    }
    return slice == nullptr ? nullptr : data(slice);
  }

  size_t count() const noexcept {
    size_t count = 0;
    for(Slice * slice = head_; slice != nullptr; slice = slice->next) {
      ++count;
    }
    return count;
  }

  void clear() noexcept {
    while(head_ != nullptr) {
      Slice * next = head_->next;
      deallocate(head_);
      head_ = next;
    }
  }

  void prepend(SliceChain & other) noexcept {
    Slice * head = other.head_;
    if(head == nullptr) {
      return;
    }
    other.head_ = nullptr;
    Slice * tail = head;
    while(tail->next != nullptr) {
      tail = tail->next;
    }
    tail->next = head_;
    head_ = head;
  }
};

struct RecordTable {
  std::unique_ptr<const char *[]> ptrs_;
  std::unique_ptr<int[]> lens_;
  std::unique_ptr<cetype_ext_t[]> encs_;
  size_t vector_length_;
  size_t capacity_;

  RecordTable() noexcept
    : ptrs_(), lens_(), encs_(), vector_length_(0), capacity_(0) {}

  explicit RecordTable(size_t vector_length)
    : RecordTable() {
    reserve(vector_length);
    vector_length_ = vector_length;
    for(size_t i = 0; i < vector_length; ++i) {
      set_na(i);
    }
  }

  RecordTable(const RecordTable & other)
    : RecordTable() {
    reserve(other.capacity_);
    vector_length_ = other.vector_length_;
    if(vector_length_ > 0) {
      std::copy(other.ptrs_.get(), other.ptrs_.get() + vector_length_, ptrs_.get());
      std::copy(other.lens_.get(), other.lens_.get() + vector_length_, lens_.get());
      std::copy(other.encs_.get(), other.encs_.get() + vector_length_, encs_.get());
    }
  }

  RecordTable & operator=(const RecordTable & other) {
    RecordTable tmp(other);
    *this = std::move(tmp);
    return *this;
  }

  RecordTable(RecordTable && other) noexcept
    : ptrs_(std::move(other.ptrs_)),
      lens_(std::move(other.lens_)),
      encs_(std::move(other.encs_)),
      vector_length_(other.vector_length_),
      capacity_(other.capacity_) {
    other.vector_length_ = 0;
    other.capacity_ = 0;
  }

  RecordTable & operator=(RecordTable && other) noexcept {
    if(this != &other) {
      ptrs_ = std::move(other.ptrs_);
      lens_ = std::move(other.lens_);
      encs_ = std::move(other.encs_);
      vector_length_ = other.vector_length_;
      capacity_ = other.capacity_;
      other.vector_length_ = 0;
      other.capacity_ = 0;
    }
    return *this;
  }

  size_t size() const noexcept { return vector_length_; }
  size_t capacity() const noexcept { return capacity_; }
  const char ** ptrs() noexcept { return ptrs_.get(); }
  const char * const * ptrs() const noexcept { return ptrs_.get(); }
  int * lengths() noexcept { return lens_.get(); }
  const int * lengths() const noexcept { return lens_.get(); }
  cetype_ext_t * encodings() noexcept { return encs_.get(); }
  const cetype_ext_t * encodings() const noexcept { return encs_.get(); }

  charport_byteview byteview(size_t i) const noexcept {
    return make_byteview(ptrs_[i], lens_[i]);
  }

  charport_strview view(size_t i) const noexcept {
    return make_strview(ptrs_[i], lens_[i], encs_[i]);
  }

  int length(size_t i) const noexcept { return lens_[i]; }
  cetype_ext_t encoding(size_t i) const noexcept { return encs_[i]; }

  void set(size_t i, const char * ptr, int len, cetype_ext_t enc) noexcept {
    ptrs_[i] = ptr;
    lens_[i] = len;
    encs_[i] = enc;
  }

  void set_na(size_t i) noexcept {
    set(i, nullptr, NA_INTEGER, CETYPE_EXT_NA);
  }

  void reserve(size_t new_capacity) {
    if(new_capacity <= capacity_) {
      return;
    }
    if(new_capacity > max_capacity()) {
      throw std::length_error("charvec record table capacity exceeds maximum");
    }

    std::unique_ptr<const char *[]> ptrs(new const char *[new_capacity]);
    std::unique_ptr<int[]> lens(new int[new_capacity]);
    std::unique_ptr<cetype_ext_t[]> encs(new cetype_ext_t[new_capacity]);
    if(vector_length_ > 0) {
      std::copy(ptrs_.get(), ptrs_.get() + vector_length_, ptrs.get());
      std::copy(lens_.get(), lens_.get() + vector_length_, lens.get());
      std::copy(encs_.get(), encs_.get() + vector_length_, encs.get());
    }
    ptrs_.swap(ptrs);
    lens_.swap(lens);
    encs_.swap(encs);
    capacity_ = new_capacity;
  }

  void reserve_for_append() {
    if(vector_length_ < capacity_) {
      return;
    }
    const size_t maximum = max_capacity();
    const size_t next = capacity_ == 0
      ? 1
      : (capacity_ > maximum / 2 ? maximum : capacity_ * 2);
    if(next <= capacity_) {
      throw std::length_error("charvec record table capacity exceeds maximum");
    }
    reserve(next);
  }

  void push_back(const charport_strview & value) {
    reserve_for_append();
    push_back_reserved(value);
  }

  // Precondition: reserve_for_append() has succeeded. GrowableBuilder uses
  // this form after reserving metadata but before committing payload bytes.
  void push_back_reserved(const charport_strview & value) noexcept {
    set(vector_length_, value.ptr, value.len, value.enc);
    ++vector_length_;
  }

private:
  static size_t max_capacity() noexcept {
    const size_t maximum = std::numeric_limits<size_t>::max();
    return std::min(
      static_cast<size_t>(R_XLEN_T_MAX),
      std::min(
        maximum / sizeof(const char *),
        std::min(maximum / sizeof(int), maximum / sizeof(cetype_ext_t))));
  }
};

} // namespace components

class Store {
public:
  components::SliceChain slices;
  components::RecordTable records;

  Store() noexcept : slices(), records() {}

  Store(size_t vector_size, size_t initial_slice_bytes)
    : slices(), records(vector_size) {
    if(initial_slice_bytes > 0) {
      slices.push_front(initial_slice_bytes);
    }
  }

  Store(const Store &) = delete;
  Store & operator=(const Store &) = delete;
  Store(Store &&) noexcept = default;
  Store & operator=(Store &&) noexcept = default;

  static Store scalar(const char * src, size_t nbytes, cetype_ext_t enc) {
    if(src == nullptr || enc == CETYPE_EXT_NA) {
      Store out(1, 0);
      out.records.set_na(0);
      return out;
    }

    const int len = components::checked_string_size(nbytes, "stored string length");
    Store out(1, nbytes);
    if(len == 0) {
      out.records.set(0, components::empty_data(), 0, enc);
    } else {
      char * dest = out.slices.front_data();
      std::memcpy(dest, src, nbytes);
      out.records.set(0, dest, len, enc);
    }
    return out;
  }

  size_t size() const noexcept { return records.size(); }
  charport_strview view(size_t idx) const noexcept { return records.view(idx); }
  charport_byteview byteview(size_t idx) const noexcept { return records.byteview(idx); }
  int length(size_t idx) const noexcept { return records.length(idx); }
  cetype_ext_t encoding(size_t idx) const noexcept { return records.encoding(idx); }
};

} // namespace charvec
} // namespace charport

#endif
