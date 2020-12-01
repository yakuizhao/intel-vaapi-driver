/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "shared.rsh"

typedef struct user_t {
  uchar4 ans;
  uint x;
  uint y;
  rs_allocation alloc;
} user;

uchar4 expect_ans;
uint expect_x;
uint expect_y;

uint32_t expect_dAlloc_GetDimX;
int expect_dXOff;
int expect_dMip;
int expect_count;
uint32_t expect_sAlloc_GetDimX;
int expect_sXOff;
int expect_sMip;

static bool failed = false;

// See http://b/21597073 "Broken structure layout for RenderScript on 32-bit/64-bit compiles"
void root(uchar4 *output, const user * usr, uint x, uint y) {
  if (!x && !y) {
    // Only check one coordinate, so as to avoid contention on global variable "failed"
    _RS_ASSERT(usr->ans.x == expect_ans.x);
    _RS_ASSERT(usr->ans.y == expect_ans.y);
    _RS_ASSERT(usr->ans.z == expect_ans.z);
    _RS_ASSERT(usr->ans.w == expect_ans.w);
    _RS_ASSERT(usr->x == expect_x);
    _RS_ASSERT(usr->y == expect_y);
  }

  uchar4 * e_in = (uchar4*)rsGetElementAt(usr->alloc, x, y);
  *output = *e_in;
}

// See http://b/32780232 "Corrupted rs_allocation instances when passed as arguments to invocables"
void args(rs_allocation dAlloc, int dXOff, int dMip, int count,
          rs_allocation sAlloc, int sXOff, int sMip) {
  _RS_ASSERT(rsIsObject(dAlloc) &&
             (rsAllocationGetDimX(dAlloc) == expect_dAlloc_GetDimX));
  _RS_ASSERT(dXOff == expect_dXOff);
  _RS_ASSERT(dMip == expect_dMip);
  _RS_ASSERT(count == expect_count);
  _RS_ASSERT(rsIsObject(sAlloc) &&
             (rsAllocationGetDimX(sAlloc) == expect_sAlloc_GetDimX));
  _RS_ASSERT(sXOff == expect_sXOff);
  _RS_ASSERT(sMip == expect_sMip);
}

void check_asserts() {
  if (failed) {
    rsSendToClientBlocking(RS_MSG_TEST_FAILED);
  }
  else {
    rsSendToClientBlocking(RS_MSG_TEST_PASSED);
  }
}
