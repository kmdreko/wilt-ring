////////////////////////////////////////////////////////////////////////////////
// FILE: ring.cpp
// DATE: 2016-02-25
// AUTH: Trevor Wilson
// DESC: Implements a lock-free, multi-consumer, multi-producer ring buffer 
//       class

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

#include "ring.h"
using namespace wilt;

#include <cstring>
// - std::memcpy

Ring_::Ring_(std::size_t size)
  : beg_(new char[size])
  , end_(beg_ + size)
{
  used_.store(0);
  free_.store(size);

  rbuf_.store(beg_);
  rptr_.store(beg_);
  wptr_.store(beg_);
  wbuf_.store(beg_);
}

Ring_::Ring_(Ring_&& r)
  : beg_(r.beg_)
  , end_(r.end_)
{
  used_.store(r.used_.load());
  free_.store(r.free_.load());

  rbuf_.store(r.rbuf_.load());
  rptr_.store(r.rptr_.load());
  wptr_.store(r.wptr_.load());
  wbuf_.store(r.wbuf_.load());

  r.beg_ = nullptr;
  r.end_ = nullptr;
  r.used_.store(0);
  r.free_.store(0);
}

Ring_::~Ring_()
{
  delete [] beg_;
}

std::ptrdiff_t Ring_::size() const
{
  return used_.load();
}

std::ptrdiff_t Ring_::capacity() const
{
  return end_ - beg_;
}

void Ring_::read(void* data, std::size_t length)
{
  char* block = acquire_read_block_(length);

  copy_read_block_(block, (char*)data, length);
  release_read_block_(block, length);
}

void Ring_::write(const void* data, std::size_t length)
{
  char* block = acquire_write_block_(length);

  copy_write_block_(block, (const char*)data, length);
  release_write_block_(block, length);
}

bool Ring_::try_read(void* data, std::size_t length)
{
  char* block = try_acquire_read_block_(length);
  if (block == nullptr)
    return false;

  copy_read_block_(block, (char*)data, length);
  release_read_block_(block, length);

  return true;
}

bool Ring_::try_write(const void* data, std::size_t length)
{
  char* block = try_acquire_write_block_(length);
  if (block == nullptr)
    return false;

  copy_write_block_(block, (const char*)data, length);
  release_write_block_(block, length);

  return true;
}

char* Ring_::normalize_(char* ptr)
{
  return ptr < end_ ? ptr : ptr - capacity();
}

char* Ring_::acquire_read_block_(std::size_t length)
{
  while (true)                                              // loop till success
  {
    char* old_rptr = rptr_.load();                          // read rptr
    while (used_.load() < (std::ptrdiff_t)length);          // not enough data

    char* new_rptr = normalize_(old_rptr + length);         // get block end
    used_.fetch_sub(length);                                // reserve
    if (!rptr_.compare_exchange_strong(old_rptr, new_rptr)) // try commit
      used_.fetch_add(length);                              // un-reserve
    else                                                    // committed
      return old_rptr;                                      // return start
  }
}

char* Ring_::try_acquire_read_block_(std::size_t length)
{
  while (true)                                              // loop till success
  {
    char* old_rptr = rptr_.load();                          // read rptr
    if (used_.load() < (std::ptrdiff_t)length)              // not enough data
      return nullptr;

    char* new_rptr = normalize_(old_rptr + length);         // get block end
    used_.fetch_sub(length);                                // reserve
    if (!rptr_.compare_exchange_strong(old_rptr, new_rptr)) // try commit
      used_.fetch_add(length);                              // un-reserve
    else                                                    // committed
      return old_rptr;                                      // return start
  }
}

void Ring_::copy_read_block_(const char* block, char* data, std::size_t length)
{
  if (block + length < end_)
  {
    std::memcpy(data, block, length);
  }
  else
  {
    auto first = end_ - block;
    std::memcpy(data, block, first);
    std::memcpy(data + first, beg_, length - first);
  }
}

void Ring_::release_read_block_(char* old_rptr, std::size_t length)
{
  char* new_rptr = normalize_(old_rptr + length);           // get block end
  while (rbuf_.load() != old_rptr);                         // wait for reads
  rbuf_.store(new_rptr);                                    // finish commit
  free_.fetch_add(length);                                  // add to free space
}

char* Ring_::acquire_write_block_(std::size_t length)
{
  while (true)                                              // loop till success
  {
    char* old_wbuf = wbuf_.load();                          // read wbuf
    while (free_.load() < (std::ptrdiff_t)length);          // not enough space

    char* new_wbuf = normalize_(old_wbuf + length);         // get block end
    free_.fetch_sub(length);                                // reserve
    if (!wbuf_.compare_exchange_strong(old_wbuf, new_wbuf)) // try commit
      free_.fetch_add(length);                              // un-reserve
    else                                                    // committed
      return old_wbuf;                                      // start writing
  }
}

char* Ring_::try_acquire_write_block_(std::size_t length)
{
  while (true)                                              // loop till success
  {
    char* old_wbuf = wbuf_.load();                          // read wbuf
    if (free_.load() < (std::ptrdiff_t)length)              // not enough space
      return nullptr;

    char* new_wbuf = normalize_(old_wbuf + length);         // get block end
    free_.fetch_sub(length);                                // reserve
    if (!wbuf_.compare_exchange_strong(old_wbuf, new_wbuf)) // try commit
      free_.fetch_add(length);                              // un-reserve
    else                                                    // committed
      return old_wbuf;                                      // start writing
  }
}

void Ring_::copy_write_block_(char* block, const char* data, std::size_t length)
{
  if (block + length < end_)
  {
    std::memcpy(block, data, length);
  }
  else
  {
    auto first = end_ - block;
    std::memcpy(block, data, first);
    std::memcpy(beg_, data + first, length - first);
  }
}

void Ring_::release_write_block_(char* old_wbuf, std::size_t length)
{
  char* new_wbuf = normalize_(old_wbuf + length);           // get block end
  while (wptr_.load() != old_wbuf);                         // wait for writes
  wptr_.store(new_wbuf);                                    // finish commit
  used_.fetch_add(length);                                  // add to used space
}
