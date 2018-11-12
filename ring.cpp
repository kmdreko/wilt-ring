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
  char* block = read_get_(length);

  read_raw_(block, (char*)data, length);
  read_end_(block, length);
}

void Ring_::write(const void* data, std::size_t length)
{
  char* block = write_get_(length);

  write_raw_(block, (const char*)data, length);
  write_end_(block, length);
}

bool Ring_::try_read(void* data, std::size_t length)
{
  char* block = read_try_(length);
  if (block == nullptr)
    return false;

  read_raw_(block, (char*)data, length);
  read_end_(block, length);

  return true;
}

bool Ring_::try_write(const void* data, std::size_t length)
{
  char* block = write_try_(length);
  if (block == nullptr)
    return false;

  write_raw_(block, (const char*)data, length);
  write_end_(block, length);

  return true;
}

char* Ring_::normalize_(char* ptr)
{
  return ptr < end_ ? ptr : ptr - capacity();
}

char* Ring_::read_get_(std::size_t length)
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

char* Ring_::read_try_(std::size_t length)
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

void Ring_::read_raw_(const char* internal, char* external, std::size_t length)
{
  if (internal + length < end_)
  {
    std::memcpy(external, internal, length);
  }
  else
  {
    auto first = end_ - internal;
    std::memcpy(external, internal, first);
    std::memcpy(external + first, beg_, length - first);
  }
}

void Ring_::read_end_(char* old_rptr, std::size_t length)
{
  char* new_rptr = normalize_(old_rptr + length);           // get block end
  while (rbuf_.load() != old_rptr);                         // wait for reads
  rbuf_.store(new_rptr);                                    // finish commit
  free_.fetch_add(length);                                  // add to free space
}

char* Ring_::write_get_(std::size_t length)
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

char* Ring_::write_try_(std::size_t length)
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

void Ring_::write_raw_(char* internal, const char* external, std::size_t length)
{
  if (internal + length < end_)
  {
    std::memcpy(internal, external, length);
  }
  else
  {
    auto first = end_ - internal;
    std::memcpy(internal, external, first);
    std::memcpy(beg_, external + first, length - first);
  }
}

void Ring_::write_end_(char* old_wbuf, std::size_t length)
{
  char* new_wbuf = normalize_(old_wbuf + length);           // get block end
  while (wptr_.load() != old_wbuf);                         // wait for writes
  wptr_.store(new_wbuf);                                    // finish commit
  used_.fetch_add(length);                                  // add to used space
}
