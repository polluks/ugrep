/******************************************************************************\
* Copyright (c) 2019, Robert van Engelen, Genivia Inc. All rights reserved.    *
*                                                                              *
* Redistribution and use in source and binary forms, with or without           *
* modification, are permitted provided that the following conditions are met:  *
*                                                                              *
*   (1) Redistributions of source code must retain the above copyright notice, *
*       this list of conditions and the following disclaimer.                  *
*                                                                              *
*   (2) Redistributions in binary form must reproduce the above copyright      *
*       notice, this list of conditions and the following disclaimer in the    *
*       documentation and/or other materials provided with the distribution.   *
*                                                                              *
*   (3) The name of the author may not be used to endorse or promote products  *
*       derived from this software without specific prior written permission.  *
*                                                                              *
* THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED *
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF         *
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO   *
* EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,       *
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, *
* PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;  *
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,     *
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR      *
* OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF       *
* ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                                   *
\******************************************************************************/

/**
@file      output.hpp
@brief     Output management
@author    Robert van Engelen - engelen@genivia.com
@copyright (c) 2019-2023, Robert van Engelen, Genivia Inc. All rights reserved.
@copyright (c) BSD-3 License - see LICENSE.txt
*/

#ifndef OUTPUT_HPP
#define OUTPUT_HPP

#include "ugrep.hpp"
#include <reflex/bits.h>
#include <reflex/matcher.h>
#include <reflex/fuzzymatcher.h>
#include <list>
#include <mutex>
#include <condition_variable>
#include <cstdint>

// max hexadecimal columns of bytes per line = 8*8
#ifndef MAX_HEX_COLUMNS
# define MAX_HEX_COLUMNS 64
#endif

// output buffering and synchronization
class Output {

 protected:

  static constexpr size_t SIZE = 16384;          // size of each buffer in the buffers container
  static constexpr size_t STOP = UNDEFINED_SIZE; // if last == STOP, cancel output
  static constexpr int FLUSH   = 1;              // mode bit: flush each line of output
  static constexpr int HOLD    = 2;              // mode bit: hold output
  static constexpr int BINARY  = 4;              // mode bit: binary file found

  struct Buffer { char data[SIZE]; }; // data buffer in the buffers container

  enum class ANSI { NA, ESC, CSI, OSC, OSC_ESC };

  typedef std::list<Buffer> Buffers; // buffers container

 public:

  // sync state to synchronize output produced by multiple threads, UNORDERED or ORDERED by slot number
  struct Sync {

    enum class Mode { UNORDERED, ORDERED };

    Sync(Mode mode)
      :
        mode(mode),
        mutex(),
        turn(),
        next(0),
        last(0),
        bits_mutex(),
        completed()
    { }

    // acquire output access
    void acquire(std::unique_lock<std::mutex> *lock, size_t slot)
    {
      switch (mode)
      {
        case Mode::UNORDERED:
          // if lock is not already acquired, acquire the lock
          if (!lock->owns_lock())
            lock->lock();
          break;

        case Mode::ORDERED:
          // if lock is not already acquired, wait for our turn to acquire the lock
          if (!lock->owns_lock())
          {
            lock->lock();
            while (last != STOP && slot != last)
              turn.wait(*lock);
          }
          break;
      }
    }

    // try to acquire output access
    bool try_acquire(std::unique_lock<std::mutex> *lock)
    {
      switch (mode)
      {
        case Mode::UNORDERED:
          // lock is owned or is available to be acquired
          return lock->owns_lock() || lock->try_lock();

        case Mode::ORDERED:
          // lock is owned
          return lock->owns_lock();
      }

      return false;
    }

    // release output access in UNORDERED mode, otherwise do nothing (until finish() is called later)
    void release(std::unique_lock<std::mutex> *lock)
    {
      switch (mode)
      {
        case Mode::UNORDERED:
          // if lock is owned, release it
          if (lock->owns_lock())
            lock->unlock();
          break;

        case Mode::ORDERED:
          break;
      }
    }

    // release output access in ORDERED mode, otherwise do nothing
    void finish(std::unique_lock<std::mutex> *lock, size_t slot)
    {
      switch (mode)
      {
        case Mode::UNORDERED:
          break;

        case Mode::ORDERED:
        {
          // if this is our slot, bump last to allow next turn, release lock, and notify other threads
          std::unique_lock<std::mutex> lock_bits(bits_mutex);

          if (last == STOP)
          {
            if (lock->owns_lock())
              lock->unlock();

            turn.notify_all();
          }
          else if (slot == last)
          {
            if (!lock->owns_lock())
              lock->lock();

            do
            {
              ++last;
              completed.rshift();
            } while (completed[0]);

            lock->unlock();

            turn.notify_all();
          }
          else
          {
            // threads without output may run ahead but must mark off their completion
            completed.insert(slot - last);
          }
          break;
        }
      }
    }

    // cancel sync, release all threads waiting on their turn in ORDERED mode
    void cancel()
    {
      switch (mode)
      {
        case Mode::UNORDERED:
          last = STOP;
          break;

        case Mode::ORDERED:
        {
          // set last to STOP to cancel sync in ORDERED mode
          std::unique_lock<std::mutex> lock_bits(bits_mutex);

          last = STOP;

          lock_bits.unlock();

          turn.notify_all();
          break;
        }
      }
    }

    // true if sync was cancelled
    bool cancelled()
    {
      return last == STOP;
    }

    Mode                         mode;       // UNORDERED or ORDERED (--sort by slot) mode
    std::mutex                   mutex;      // mutex to synchronize output
    std::condition_variable      turn;       // ORDERED: cv for threads to take turns by checking if last slot is their slot
    size_t                       next;       // ORDERED: next slot assigned to thread
    std::atomic_size_t           last;       // ORDERED: slot for threads to wait for their turn to output, or STOP to cancel
    std::mutex                   bits_mutex; // ORDERED: mutex to synchronize bitset access and when setting last = STOP
    reflex::Bits                 completed;  // ORDERED: bitset of completed slots marked by release() by threads that don't acquire() output

  };

  // hex dump state
  struct Dump {

    // hex dump mode for color highlighting
    static constexpr short HEX_MATCH         = 0;
    static constexpr short HEX_LINE          = 1;
    static constexpr short HEX_CONTEXT_MATCH = 2;
    static constexpr short HEX_CONTEXT_LINE  = 3;
    static constexpr short HEX_MAX           = 4;

    // hex color highlights for HEX_MATCH, HEX_LINE, HEX_CONTEXT_MATCH, HEX_CONTEXT_LINE
    static const char *color_hex[HEX_MAX];

    // constructor
    Dump(Output& out)
      :
        out(out),
        offset(0)
    {
      done();
    }

    // dump matching data in hex, mode is
    void hex(short mode, size_t byte_offset, const char *data, size_t size);

    // jump to the next hex dump location (option -o)
    inline void next(size_t byte_offset)
    {
      if (offset - offset % flag_hex_columns != byte_offset - byte_offset % flag_hex_columns)
        done();
    }

    // hex line is incomplete: to complete invoke done()
    bool incomplete()
    {
      return offset % flag_hex_columns != 0;
    }

    // if hex line is incomplete: complete it with done()
    inline void complete(size_t off)
    {
      if (offset > 0 && offset < off)
        done();
    }

    // done dumping hex
    inline void done()
    {
      if (incomplete())
      {
        line();
        offset += flag_hex_columns - 1;
        offset -= offset % flag_hex_columns;
      }
      for (int i = 0; i < MAX_HEX_COLUMNS; ++i)
        prevb[i] = bytes[i] = -1;
    }

    // dump one line of hex
    void line();

    Output& out;                    // reference to the output state of this hex dump state
    size_t  offset;                 // current byte offset in the hex dump
    short   bytes[MAX_HEX_COLUMNS]; // one line of hex dump bytes with their mode bits for color highlighting
    short   prevb[MAX_HEX_COLUMNS]; // previously displayed bytes[], to produce line with *
    bool    pstar;                  // previously output a *

  };

  // global directory tree state for output, protected by acquire()
  struct Tree {

    static const char  *bar;    // fixed string to display a vertical line
    static const char  *ptr;    // fixed string to display a vertical line and connector
    static const char  *end;    // fixed string to display a vertical line ending

    static std::string  path;   // tree directory path buffer
    static int          depth;  // tree directory depth

  };

  // constructor
  Output(FILE *file)
    :
      file(file),
      eof(false),
      sync(NULL),
      dump(*this),
      lock_(NULL),
      slot_(0),
      lineno_(0),
      mode_(flag_line_buffered ? FLUSH : 0),
      cols_(0),
      ansi_(ANSI::NA),
      skip_(false)
  {
    grow();
  }

  // destructor
  ~Output()
  {
    flush();
    if (lock_ != NULL)
      delete lock_;
  }

  // output a character c
  inline void chr(int c)
  {
    if (cur_ >= buf_->data + SIZE)
      next();
    *cur_++ = c;
  }

  // output a std::string s
  inline void str(const std::string& s)
  {
    str(s.c_str(), s.size());
  }

  // output a \0-terminated string s
  inline void str(const char *s)
  {
    if (s[0] != '\0')
    {
      if (s[1] != '\0')
        str(s, strlen(s));
      else
        chr(s[0]);
    }
  }

  // output a string s of byte length n
  inline void str(const char *s, size_t n)
  {
    while (cur_ + n >= buf_->data + SIZE)
    {
      size_t k = static_cast<size_t>(buf_->data + SIZE - cur_);
      memcpy(cur_, s, k);
      s += k;
      n -= k;
      cur_ += k;
      next();
    }
    memcpy(cur_, s, n);
    cur_ += n;
  }

  // output a UTF-8 multibyte string s of byte length n for up to k UTF-8-encoded characters
  inline void utf8strn(const char *s, size_t n, size_t k)
  {
    const char *t = s;
    while (n-- > 0 && k-- > 0)
      while (static_cast<uint8_t>(*++s & 0xc0) == 0x80 && n > 0)
        --n;
    str(t, s - t);
  }

  // output a UTF-8 multibyte string s (\0-terminated) for up to k Unicode characters
  inline void utf8str(const char *s, size_t k)
  {
    const char *t = s;
    while (*s != '\0' && k-- > 0)
      while (static_cast<uint8_t>(*++s & 0xc0) == 0x80)
        continue;
    str(t, s - t);
  }

  // output a URI-encoded string s
  inline void uri(const std::string& s)
  {
    uri(s.c_str());
  }

  // output a URI-encoded string s
  inline void uri(const char *s)
  {
    while (*s != '\0')
    {
      if (*s >= 0x20 && *s <= 0x7e && *s != '%' && *s != ';')
      {
        chr(*s++);
      }
      else
      {
        chr('%');
        hex(static_cast<uint8_t>(*s++), 2);
      }
    }
  }

  // output a match
  inline void mat(reflex::AbstractMatcher *matcher)
  {
    if (flag_only_matching)
    {
      str(matcher->begin(), matcher->size());
    }
    else
    {
      const char *e = matcher->eol(); // warning: must call eol() before bol()
      const char *b = matcher->bol();
      str(b, e - b);
    }
  }

  // output a quoted match with escapes for \ and "
  inline void quote(reflex::AbstractMatcher *matcher)
  {
    if (flag_only_matching)
    {
      quote(matcher->begin(), matcher->size());
    }
    else
    {
      const char *e = matcher->eol(); // warning: must call eol() before bol()
      const char *b = matcher->bol();
      quote(b, e - b);
    }
  }

  // output a match as a string in C/C++
  inline void cpp(reflex::AbstractMatcher *matcher)
  {
    if (flag_only_matching)
    {
      cpp(matcher->begin(), matcher->size());
    }
    else
    {
      const char *e = matcher->eol(); // warning: must call eol() before bol()
      const char *b = matcher->bol();
      cpp(b, e - b);
    }
  }

  // output a match as a quoted string in CSV
  inline void csv(reflex::AbstractMatcher *matcher)
  {
    if (flag_only_matching)
    {
      csv(matcher->begin(), matcher->size());
    }
    else
    {
      const char *e = matcher->eol(); // warning: must call eol() before bol()
      const char *b = matcher->bol();
      csv(b, e - b);
    }
  }

  // output a match as a quoted string in JSON
  inline void json(reflex::AbstractMatcher *matcher)
  {
    if (flag_only_matching)
    {
      json(matcher->begin(), matcher->size());
    }
    else
    {
      const char *e = matcher->eol(); // warning: must call eol() before bol()
      const char *b = matcher->bol();
      json(b, e - b);
    }
  }

  // output a match in XML
  inline void xml(reflex::AbstractMatcher *matcher)
  {
    if (flag_only_matching)
    {
      xml(matcher->begin(), matcher->size());
    }
    else
    {
      const char *e = matcher->eol(); // warning: must call eol() before bol()
      const char *b = matcher->bol();
      xml(b, e - b);
    }
  }

  // output an unsigned integer i with field width w (padded with spaces)
  inline void num(size_t i, size_t w = 1)
  {
    char tmp[24];
    size_t k = sizeof(tmp);

    do
      tmp[--k] = i % 10 + '0';
    while ((i /= 10) > 0);

    size_t n = sizeof(tmp) - k;

    while (w-- > n)
      chr(' ');

    str(&tmp[k], n);
  }

  // output a number in hex with width w (padded with digit '0')
  inline void hex(size_t i, size_t w = 1)
  {
    char tmp[16];
    size_t k = sizeof(tmp);

    do
      tmp[--k] = "0123456789abcdef"[i & 0xf];
    while ((i >>= 4) > 0);

    size_t n = sizeof(tmp) - k;

    while (w-- > n)
      chr('0');

    str(&tmp[k], n);
  }

  // output a single byte in octal
  inline void oct(int i)
  {
    uint8_t b = static_cast<uint8_t>(i);
    chr('0' + (b >> 6));
    chr('0' + ((b >> 3) & 7));
    chr('0' + (b & 7));
  }

  // output a newline (platform-specific conditional "\r\n" or "\n"); flush if --line-buffered
  inline void nl(bool lf_only = false)
  {
    if (!lf_only)
#ifdef OS_WIN
      chr('\r');
#else
      { /* do not emit \r */ }
#endif
    chr('\n');
    check_flush();
  }

  // enable line buffered mode to flush each line to output
  void set_flush()
  {
    mode_ |= FLUSH;
  }

  // flush if output is line buffered to flush each line
  void check_flush()
  {
    if (mode_ == FLUSH)
      flush();
  }

  // hold the output and do not flush, buffer all output until further notice
  void hold()
  {
    mode_ |= HOLD;
  }

  // launch output when held back
  void launch()
  {
    if ((mode_ & HOLD) != 0)
    {
      mode_ &= ~HOLD;
      check_flush();
    }
  }

  // return true when holding the output
  bool holding()
  {
    return mode_ & HOLD;
  }

  // synchronize output by threads on the given sync object
  void sync_on(Sync *s)
  {
    sync = s;
    if (lock_ != NULL)
      delete lock_;
    lock_ = new std::unique_lock<std::mutex>(sync->mutex, std::defer_lock);
  }

  // start synchronizing output for this slot in ORDERED mode (--sort)
  void begin(size_t slot)
  {
    slot_ = slot;
  }

  // acquire output synchronization lock
  void acquire()
  {
    if (sync != NULL)
      sync->acquire(lock_, slot_);
  }

  // acquire lock and flush the buffers, if not held back
  void flush()
  {
    if (buf_ != buffers_.begin() || cur_ > buf_->data)
    {
      if (!eof)
      {
        // if multi-threaded and lock is not owned already, then lock on master's mutex
        acquire();

        // flush the buffers container to the designated output file, pipe, or stream
        for (Buffers::iterator i = buffers_.begin(); i != buf_; ++i)
        {
          if (flag_width == 0)
          {
            size_t nwritten = fwrite(i->data, 1, SIZE, file);
            if (nwritten < SIZE)
            {
              cancel();
              break;
            }
          }
          else
          {
            if (flush_truncated_lines(i->data, SIZE))
            {
              cancel();
              break;
            }
          }
        }

        if (!eof)
        {
          size_t num = cur_ - buf_->data;

          if (num > 0)
          {
            if (flag_width == 0)
            {
              size_t nwritten = fwrite(buf_->data, 1, num, file);
              if (nwritten < num)
                cancel();
            }
            else
            {
              if (flush_truncated_lines(buf_->data, num))
                cancel();
            }
          }

          if (!eof && fflush(file) != 0)
            cancel();
        }
      }

      buf_ = buffers_.begin();
      cur_ = buf_->data;
    }
  }

  // flush a block of data as truncated lines limited to --width columns
  bool flush_truncated_lines(const char *data, size_t size);

  // discard buffered output
  void discard()
  {
    buf_ = buffers_.begin();
    cur_ = buf_->data;
  }

  // flush output and release sync slot, if one was assigned with sync_on()
  void release()
  {
    if ((mode_ & HOLD) == 0)
      flush();
    else
      discard();

    mode_ = flag_line_buffered ? FLUSH : 0;

    if (sync != NULL)
      sync->release(lock_);
  }

  // end output in ORDERED mode (--sort)
  void end()
  {
    if (sync != NULL)
      sync->finish(lock_, slot_);
  }

  // cancel output
  void cancel()
  {
    eof = true;

    if (sync != NULL)
      sync->cancel();
  }

  // true if output was cancelled()
  bool cancelled()
  {
    return sync != NULL && sync->cancelled();
  }

  // output the header part of the match, preceding the matched line
  void header(const char *pathname, const std::string& partname, bool& heading, size_t lineno, reflex::AbstractMatcher *matcher, size_t byte_offset, const char *sep, bool newline);

  // output the pathname header for --files_with_matches and --count
  void header(const char *pathname, const std::string& partname);

  // output "Binary file ... matches"
  void binary_file_matches(const char *pathname, const std::string& partname);

  // output format with option --format-begin and --format-end
  void format(const char *format, size_t matches);

  // output formatted match with options --format, --format-open, --format-close, returns false when nothing is output
  bool format(const char *format, const char *pathname, const std::string& partname, size_t matches, size_t *matching, reflex::AbstractMatcher *matcher, bool& heading, bool body, bool next);

  // output formatted inverted match with options -v --format, --format-open, --format-close
  void format_invert(const char *format, const char *pathname, const std::string& partname, size_t matches, size_t lineno, size_t offset, const char *ptr, size_t size, bool& heading, bool next);

  // output a quoted string with escapes for \ and "
  void quote(const char *data, size_t size);

  // output quoted string in C/C++
  void cpp(const char *data, size_t size);

  // output quoted string in CSV
  void csv(const char *data, size_t size);

  // output quoted string in JSON
  void json(const char *data, size_t size);

  // output in XML
  void xml(const char *data, size_t size);

 protected:

  // next buffer, allocate one if needed (when multi-threaded and lock is owned by another thread)
  void next()
  {
    if ((mode_ & HOLD) == 0 && (sync == NULL || sync->try_acquire(lock_)))
    {
      flush();
    }
    else
    {
      // allocate a new buffer if no next buffer was allocated before
      if (++buf_ == buffers_.end())
        grow();
      else
        cur_ = buf_->data;
    }
  }

  // allocate a new buffer to grow the buffers container
  void grow()
  {
    buf_ = buffers_.emplace(buffers_.end());
    cur_ = buf_->data;
  }

  // get a group capture's string pointer and size specified by %[ARG] as arg, if any
  std::pair<const char*,size_t> capture(reflex::AbstractMatcher *matcher, const char *arg);

 public:

  FILE            *file; // output stream
  std::atomic_bool eof;  // the other end closed or has an error
  Sync            *sync; // synchronization object
  Dump             dump; // hex dump state

 protected:

  std::unique_lock<std::mutex> *lock_;    // synchronization lock
  size_t                        slot_;    // current slot to take turns
  size_t                        lineno_;  // last line number matched, when --format field %u (unique) is used
  Buffers                       buffers_; // buffers container
  Buffers::iterator             buf_;     // current buffer in the container
  char                         *cur_;     // current position in the current buffer
  int                           mode_;    // bitmask 1 if line buffered 2 if hold
  size_t                        cols_;    // number of columns output so far, if --width
  ANSI                          ansi_;    // ANSI escape sequence
  bool                          skip_;    // skip until next newline in buffers, if --width

};

#endif
