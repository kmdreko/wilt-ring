////////////////////////////////////////////////////////////////////////////////
// FILE: ring.h
// DATE: 2016-02-25
// AUTH: Trevor Wilson
// DESC: Defines a lock-free, multi-consumer, multi-producer ring buffer class

////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2016 Trevor Wilson
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy 
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights 
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell 
// copies of the Software, and to permit persons to whom the Software is 
// furnished to do so, subject to the following conditions :
// 
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE 
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef WILT_RING_H
#define WILT_RING_H

#include <atomic>
// - std::atomic
#include <cstddef>
// - std::size_t
// - std::ptrdiff_t
#include <new>
// - ::new(ptr)

namespace wilt
{
  //////////////////////////////////////////////////////////////////////////////
  // This structure aims to access elements in a ring buffer from multiple
  // concurrent readers and writers in a lock-free manner.
  // 
  // The class works by allocating the array and storing two pointers (for the
  // beginning and end of the allocated space). Two atomic pointers are used to
  // track the beginning and end of the currently used storage space. To
  // facilitate concurrent reads and writes, theres a read buffer pointer before
  // the read pointer for data currently being read, and a corresponding write
  // buffer pointer beyond the write pointer for data currently being written.
  // These buffer pointers cannot overlap. Just using these pointers suffer from
  // some minute inefficiencies and a few ABA problems. Therfore, atomic
  // integers are used to store the currently used and currently free sizes.
  // 
  // It allows multiple readers and multiple writers by implementing a reserve-
  // commit system. A thread wanting to read will check the used size to see if
  // there's enough data. If there is, it subtracts from the used size to
  // 'reserve' the read. It then does a compare-exchange to 'commit' by
  // increasing the read pointer. If that fails, then it backs out ('un-
  // reserves') by adding back to the used size and tries again. If it
  // succeeds, then it proceeds to read the data. In order to complete, the
  // reader must update the read buffer pointer to where it just finished
  // reading from. However, because other readers that started before may not be
  // done yet, the reader must wait until the read buffer pointer points to
  // where the read started. Only, then is the read buffer pointer updated, and
  // the free size increased. So while this implementation is lock-free, it is
  // not wait-free. This same principle works the same when writing (ammended
  // for the appropriate pointers).
  // 
  // If two readers try to read at the same time and there is only enough data
  // for one of them. The used size MAY be negative because they both 'reserve'
  // the data. This is an over-reserved state. But the compare-exchange will
  // only allow one reader to 'commit' to the read and the other will 'un-
  // reserve' the read.
  // 
  // |beg           |rptr      used=5             |wbuf         - unused
  // |----|----|++++|====|====|====|====|====|++++|----|        + modifying
  //  free=3   |rbuf                         |wptr     |end     = used
  // 
  // The diagram above shows a buffer of size 10 storing 5 bytes with a reader
  // reading one byte and one writer reading one byte.
  // 
  // Out of the box, the class works by reading and writing raw bytes from POD
  // data types and arrays. A wrapper could allow for a nicer interface for
  // pushing and popping elements. As it stands, this structure cannot be easily
  // modified to store types of variable size.

  class Ring_
  {
  private:
    ////////////////////////////////////////////////////////////////////////////
    // TYPE DEFINITIONS
    ////////////////////////////////////////////////////////////////////////////

    typedef char*                       data_ptr;
    typedef std::atomic<std::ptrdiff_t> size_type;
    typedef std::atomic<char*>          atom_ptr;

  private:
    ////////////////////////////////////////////////////////////////////////////
    // PRIVATE MEMBERS
    ////////////////////////////////////////////////////////////////////////////
    // Beginning and end pointers don't need to be atomic because they don't 
    // change. used_ and free_ can be negative in certain cases (and that's ok).

    data_ptr  beg_;  // pointer to beginning of data block
    data_ptr  end_;  // pointer to end of data block
    size_type used_; // size of unreserved used space
    size_type free_; // size of unreserved free space
    atom_ptr  rbuf_; // pointer to beginning of data being read
    atom_ptr  rptr_; // pointer to beginning of data
    atom_ptr  wptr_; // pointer to end of data
    atom_ptr  wbuf_; // pointer to end of data being written

  public:
    ////////////////////////////////////////////////////////////////////////////
    // CONSTRUCTORS AND DESTRUCTORS
    ////////////////////////////////////////////////////////////////////////////

    // Constructs a ring without a buffer (capacity() == 0)
    Ring_();

    // Constructs a ring with a buffer with a size
    Ring_(std::size_t size);

    // Moves the buffer between rings, assumes no concurrent operations
    Ring_(Ring_&& ring);

    // Moves the buffer between rings, assumes no concurrent operations on
    // either ring. Deallocates the buffer
    Ring_& operator= (Ring_&& ring);

    // No copying. In the current model, there is no safe way to read without
    // committing (would have to be a destructive read). Either that, or I'd
    // have to dictate that reads and writes are not allowed during copying
    Ring_(const Ring_&)             = delete;
    Ring_& operator= (const Ring_&) = delete;

    // Deallocates the buffer
    ~Ring_();

  public:
    ////////////////////////////////////////////////////////////////////////////
    // QUERY FUNCTIONS
    ////////////////////////////////////////////////////////////////////////////
    // Functions only report on the state of the ring

    // Returns the current amount of non-reserved used space (amount of written 
    // data that a read hasn't yet reserved). May potentially return a negative 
    // number if the system is over-reserved (can occur when two or more reads 
    // try to reserve the remaining space). This, of course, doesn't report 
    // writes that have not completed. 
    std::ptrdiff_t size() const;

    // Maximum amount of data that can be held
    std::ptrdiff_t capacity() const;

  public:
    ////////////////////////////////////////////////////////////////////////////
    // ACCESSORS AND MODIFIERS
    ////////////////////////////////////////////////////////////////////////////
    // All operations assume object has not been moved. Blocking operations run
    // until operation is completed. Non-blocking operations fail if there is
    // not enough space

    void read(void* data, std::size_t length);            // blocking read
    void write(const void* data, std::size_t length);     // blocking write
    bool try_read(void* data, std::size_t length);        // non-blocking read
    bool try_write(const void* data, std::size_t length); // non-blocking write

  protected:
    ////////////////////////////////////////////////////////////////////////////
    // PROTECTED FUNCTIONS
    ////////////////////////////////////////////////////////////////////////////
    // Helper functions

    // Wraps a pointer within the array. Assumes 'beg_ <= ptr < end_+capacity()'
    char* normalize_(char*);

    char* acquire_read_block_(std::size_t length);
    char* try_acquire_read_block_(std::size_t length);
    void  copy_read_block_(const char* block, char* data, std::size_t length);
    void  release_read_block_(char* block, std::size_t length);

    char* acquire_write_block_(std::size_t length);
    char* try_acquire_write_block_(std::size_t length);
    void  copy_write_block_(char* block, const char* data, std::size_t length);
    void  release_write_block_(char* block, std::size_t length);

    char* begin_alloc_()             { return beg_;  }
    const char* begin_alloc_() const { return beg_;  }
    char* end_alloc_()               { return end_;  }
    const char* end_alloc_() const   { return end_;  }
    char* begin_data_()              { return rptr_; }
    const char* begin_data_() const  { return rptr_; }
    char* end_data_()                { return wptr_; }
    const char* end_data_() const    { return wptr_; }

  }; // class Ring_

  template <class T>
  class Ring : protected Ring_
  {
  public:
    ////////////////////////////////////////////////////////////////////////////
    // CONSTRUCTORS AND DESTRUCTORS
    ////////////////////////////////////////////////////////////////////////////

    // Ring must be constructed with a size that does not change
    Ring(std::size_t);

    // Moving assumes nothing is reading or writing (should be the case anyway).
    Ring(Ring<T>&&);

    // No default constructor, must be initialized with a size
    Ring()                         = delete;

    // No copying. In the current model, there is no safe way to read without
    // committing (would have to be a destructive read). Either that, or I'd
    // have to dictate that reads and writes are not allowed during copying
    Ring(const Ring_&)             = delete;
    Ring& operator= (const Ring_&) = delete;

    // No move assignment. I'm lazy
    Ring& operator= (Ring_&&)      = delete;

    // Deallocates the buffer, destructs stored data.
    ~Ring();

  public:
    ////////////////////////////////////////////////////////////////////////////
    // QUERY FUNCTIONS
    ////////////////////////////////////////////////////////////////////////////
    // Functions only report on the state of the ring

    // Returns the current amount of non-reserved used space (amount of written 
    // data that a read hasn't yet reserved). May potentially return a negative 
    // number if the system is over-reserved (can occur when two or more reads 
    // try to reserve the remaining space). This, of course, doesn't report 
    // writes that have not completed. 
    std::ptrdiff_t size() const;

    // Maximum amount of data that can be held
    std::ptrdiff_t capacity() const;

  public:
    ////////////////////////////////////////////////////////////////////////////
    // ACCESSORS AND MODIFIERS
    ////////////////////////////////////////////////////////////////////////////
    // All operations assume object has not been moved. Blocking operations run
    // until operation is completed. Non-blocking operations fail if there is
    // not enough space

    void read(T& data);            // blocking read
    void write(const T& data);     // blocking write
    void write(T&& data);          // blocking write
    bool try_read(T& data);        // non-blocking read
    bool try_write(const T& data); // non-blocking write
    bool try_write(T&& data);      // non-blocking write

  }; // class Ring<T>

  template <class T>
  Ring<T>::Ring(std::size_t size)
    : Ring_(size * sizeof(T))
  { }

  template <class T>
  Ring<T>::Ring(Ring<T>&& ring)
    : Ring_(std::move(ring))
  { }

  template <class T>
  Ring<T>::~Ring()
  {
    if (size() == 0)
      return;

    T* itr = (T*)begin_data_();
    T* end = (T*)end_data_();
    do
    {
      itr->~T();
      itr = (T*)normalize_((char*)++itr);
    } while (itr != end);
  }

  template <class T>
  std::ptrdiff_t Ring<T>::size() const
  {
    return Ring_::size() / sizeof(T);
  }

  template <class T>
  std::ptrdiff_t Ring<T>::capacity() const
  {
    return Ring_::capacity() / sizeof(T);
  }

  template <class T>
  void Ring<T>::read(T& data)
  {
    T* block = (T*)acquire_read_block_(sizeof(T));

    data = std::move(*block);
    block->~T();
    release_read_block_((char*)block, sizeof(T));
  }

  template <class T>
  void Ring<T>::write(const T& data)
  {
    char* block = acquire_write_block_(sizeof(T));

    new(block) T(data);
    release_write_block_(block, sizeof(T));
  }

  template <class T>
  void Ring<T>::write(T&& data)
  {
    char* block = acquire_write_block_(sizeof(T));

    new(block) T(std::move(data));
    release_write_block_(block, sizeof(T));
  }

  template <class T>
  bool Ring<T>::try_read(T& data)
  {
    T* block = (T*)try_acquire_read_block_(sizeof(T));
    if (block == nullptr)
      return false;

    data = std::move(*block);
    block->~T();
    release_read_block_((char*)block, sizeof(T));

    return true;
  }

  template <class T>
  bool Ring<T>::try_write(const T& data)
  {
    char* block = try_acquire_write_block_(sizeof(T));
    if (block == nullptr)
      return false;

    new(block) T(data);
    release_write_block_(block, sizeof(T));

    return true;
  }

  template <class T>
  bool Ring<T>::try_write(T&& data)
  {
    char* block = try_acquire_write_block_(sizeof(T));
    if (block == nullptr)
      return false;

    new(block) T(std::move(data));
    release_write_block_(block, sizeof(T));

    return true;
  }

} // namespace wilt

#endif // !WILT_RING_H