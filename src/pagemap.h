// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2005, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat <opensource@google.com>
//
// A data structure used by the caching malloc.  It maps from page# to
// a pointer that contains info about that page.  We use two
// representations: one for 32-bit addresses, and another for 64 bit
// addresses.  Both representations provide the same interface.  The
// first representation is implemented as a flat array, the seconds as
// a three-level radix tree that strips away approximately 1/3rd of
// the bits every time.
//
// The BITS parameter should be the number of bits required to hold
// a page number.  E.g., with 32 bit pointers and 4K pages (i.e.,
// page offset fits in lower 12 bits), BITS == 20.

#ifndef TCMALLOC_PAGEMAP_H_
#define TCMALLOC_PAGEMAP_H_

#include "config.h"

#include <stddef.h>                     // for size_t
#include <string.h>                     // for memset
#include <stdint.h>

#include "base/basictypes.h"
#include "internal_logging.h"  // for ASSERT

// Single-level array
template <int BITS>
class TCMalloc_PageMap1 {
 private:
  static const int LENGTH = 1 << BITS;

  void** array_;

 public:
  typedef uintptr_t Number;

  explicit TCMalloc_PageMap1(void* (*allocator)(size_t)) {
    array_ = reinterpret_cast<void**>((*allocator)(sizeof(void*) << BITS));
    memset(array_, 0, sizeof(void*) << BITS);
  }

  // Ensure that the map contains initialized entries "x .. x+n-1".
  // Returns true if successful, false if we could not allocate memory.
  bool Ensure(Number x, size_t n) {
    // Nothing to do since flat array was allocated at start.  All
    // that's left is to check for overflow (that is, we don't want to
    // ensure a number y where array_[y] would be an out-of-bounds
    // access).
    return n <= LENGTH - x;   // an overflow-free way to do "x + n <= LENGTH"
  }

  void PreallocateMoreMemory() {}

  // Return the current value for KEY.  Returns nullptr if not yet
  // set, or if k is out of range.
  ALWAYS_INLINE
  void* get(Number k) const {
    if ((k >> BITS) > 0) {
      return nullptr;
    }
    return array_[k];
  }

  // REQUIRES "k" is in range "[0,2^BITS-1]".
  // REQUIRES "k" has been ensured before.
  //
  // Sets the value 'v' for key 'k'.
  void set(Number k, void* v) {
    array_[k] = v;
  }

  // Return the first non-nullptr pointer found in this map for a page
  // number >= k.  Returns nullptr if no such number is found.
  void* Next(Number k) const {
    while (k < (1 << BITS)) {
      if (array_[k] != nullptr) return array_[k];
      k++;
    }
    return nullptr;
  }
};

// Two-level radix tree
template <int BITS>
class TCMalloc_PageMap2 {
 private:
  static const int LEAF_BITS = (BITS + 1) / 2;
  static const int LEAF_LENGTH = 1 << LEAF_BITS;

  static const int ROOT_BITS = BITS - LEAF_BITS;
  static const int ROOT_LENGTH = 1 << ROOT_BITS;

  // Leaf node
  struct Leaf {
    void* values[LEAF_LENGTH];
  };

  Leaf* root_[ROOT_LENGTH];             // Pointers to child nodes
  void* (*allocator_)(size_t);          // Memory allocator

 public:
  typedef uintptr_t Number;

  explicit TCMalloc_PageMap2(void* (*allocator)(size_t)) {
    allocator_ = allocator;
    memset(root_, 0, sizeof(root_));
  }

  ALWAYS_INLINE
  void* get(Number k) const {
    const Number i1 = k >> LEAF_BITS;
    const Number i2 = k & (LEAF_LENGTH-1);
    if ((k >> BITS) > 0 || root_[i1] == nullptr) {
      return nullptr;
    }
    return root_[i1]->values[i2];
  }

  void set(Number k, void* v) {
    const Number i1 = k >> LEAF_BITS;
    const Number i2 = k & (LEAF_LENGTH-1);
    ASSERT(i1 < ROOT_LENGTH);
    root_[i1]->values[i2] = v;
  }

  bool Ensure(Number start, size_t n) {
    for (Number key = start; key <= start + n - 1; ) {
      const Number i1 = key >> LEAF_BITS;

      // Check for overflow
      if (i1 >= ROOT_LENGTH)
        return false;

      // Make 2nd level node if necessary
      if (root_[i1] == nullptr) {
        Leaf* leaf = reinterpret_cast<Leaf*>((*allocator_)(sizeof(Leaf)));
        if (leaf == nullptr) return false;
        memset(leaf, 0, sizeof(*leaf));
        root_[i1] = leaf;
      }

      // Advance key past whatever is covered by this leaf node
      key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
    }
    return true;
  }

  void PreallocateMoreMemory() {
    // Allocate enough to keep track of all possible pages
    if (BITS < 20) {
      Ensure(0, Number(1) << BITS);
    }
  }

  void* Next(Number k) const {
    while (k < (Number(1) << BITS)) {
      const Number i1 = k >> LEAF_BITS;
      Leaf* leaf = root_[i1];
      if (leaf != nullptr) {
        // Scan forward in leaf
        for (Number i2 = k & (LEAF_LENGTH - 1); i2 < LEAF_LENGTH; i2++) {
          if (leaf->values[i2] != nullptr) {
            return leaf->values[i2];
          }
        }
      }
      // Skip to next top-level entry
      k = (i1 + 1) << LEAF_BITS;
    }
    return nullptr;
  }
};

// Three-level radix tree
template <int BITS>
class TCMalloc_PageMap3 {
 private:
  // How many bits should we consume at each interior level
  static const int INTERIOR_BITS = (BITS + 2) / 3; // Round-up
  static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;

  // How many bits should we consume at leaf level
  static const int LEAF_BITS = BITS - 2*INTERIOR_BITS;
  static const int LEAF_LENGTH = 1 << LEAF_BITS;

  // Interior node
  struct Node {
    Node* ptrs[INTERIOR_LENGTH];
  };

  // Leaf node
  struct Leaf {
    void* values[LEAF_LENGTH];
  };

  Node  root_;                          // Root of radix tree
  void* (*allocator_)(size_t);          // Memory allocator

  Node* NewNode() {
    Node* result = reinterpret_cast<Node*>((*allocator_)(sizeof(Node)));
    if (result != nullptr) {
      memset(result, 0, sizeof(*result));
    }
    return result;
  }

 public:
  typedef uintptr_t Number;

  explicit TCMalloc_PageMap3(void* (*allocator)(size_t)) {
    allocator_ = allocator;
    memset(&root_, 0, sizeof(root_));
  }

  ALWAYS_INLINE
  void* get(Number k) const {
    const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
    const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH-1);
    const Number i3 = k & (LEAF_LENGTH-1);
    if ((k >> BITS) > 0 ||
        root_.ptrs[i1] == nullptr || root_.ptrs[i1]->ptrs[i2] == nullptr) {
      return nullptr;
    }
    return reinterpret_cast<Leaf*>(root_.ptrs[i1]->ptrs[i2])->values[i3];
  }

  void set(Number k, void* v) {
    ASSERT(k >> BITS == 0);
    const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
    const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH-1);
    const Number i3 = k & (LEAF_LENGTH-1);
    reinterpret_cast<Leaf*>(root_.ptrs[i1]->ptrs[i2])->values[i3] = v;
  }

  bool Ensure(Number start, size_t n) {
    for (Number key = start; key <= start + n - 1; ) {
      const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
      const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH-1);

      // Check for overflow
      if (i1 >= INTERIOR_LENGTH || i2 >= INTERIOR_LENGTH)
        return false;

      // Make 2nd level node if necessary
      if (root_.ptrs[i1] == nullptr) {
        Node* n = NewNode();
        if (n == nullptr) return false;
        root_.ptrs[i1] = n;
      }

      // Make leaf node if necessary
      if (root_.ptrs[i1]->ptrs[i2] == nullptr) {
        Leaf* leaf = reinterpret_cast<Leaf*>((*allocator_)(sizeof(Leaf)));
        if (leaf == nullptr) return false;
        memset(leaf, 0, sizeof(*leaf));
        root_.ptrs[i1]->ptrs[i2] = reinterpret_cast<Node*>(leaf);
      }

      // Advance key past whatever is covered by this leaf node
      key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
    }
    return true;
  }

  void PreallocateMoreMemory() {
  }

  void* Next(Number k) const {
    while (k < (Number(1) << BITS)) {
      const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
      const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH-1);
      if (root_.ptrs[i1] == nullptr) {
        // Advance to next top-level entry
        k = (i1 + 1) << (LEAF_BITS + INTERIOR_BITS);
      } else {
        Leaf* leaf = reinterpret_cast<Leaf*>(root_.ptrs[i1]->ptrs[i2]);
        if (leaf != nullptr) {
          for (Number i3 = (k & (LEAF_LENGTH-1)); i3 < LEAF_LENGTH; i3++) {
            if (leaf->values[i3] != nullptr) {
              return leaf->values[i3];
            }
          }
        }
        // Advance to next interior entry
        k = ((k >> LEAF_BITS) + 1) << LEAF_BITS;
      }
    }
    return nullptr;
  }
};

#endif  // TCMALLOC_PAGEMAP_H_
