// Copyright 2012 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Author: Jyrki Alakuijala (jyrki@google.com)
//

#include <assert.h>
#include <math.h>

#include "./backward_references_enc.h"
#include "./histogram_enc.h"
#include "../dsp/lossless.h"
#include "../dsp/lossless_common.h"
#include "../dsp/dsp.h"
#include "../utils/color_cache_utils.h"
#include "../utils/utils.h"

#define VALUES_IN_BYTE 256

#define MIN_BLOCK_SIZE 256  // minimum block size for backward references

#define MAX_ENTROPY    (1e30f)

// 1M window (4M bytes) minus 120 special codes for short distances.
#define WINDOW_SIZE_BITS 20
#define WINDOW_SIZE ((1 << WINDOW_SIZE_BITS) - 120)

// Minimum number of pixels for which it is cheaper to encode a
// distance + length instead of each pixel as a literal.
#define MIN_LENGTH 4
// If you change this, you need MAX_LENGTH_BITS + WINDOW_SIZE_BITS <= 32 as it
// is used in VP8LHashChain.
#define MAX_LENGTH_BITS 12
// We want the max value to be attainable and stored in MAX_LENGTH_BITS bits.
#define MAX_LENGTH ((1 << MAX_LENGTH_BITS) - 1)
#if MAX_LENGTH_BITS + WINDOW_SIZE_BITS > 32
#error "MAX_LENGTH_BITS + WINDOW_SIZE_BITS > 32"
#endif

// -----------------------------------------------------------------------------

static const uint8_t plane_to_code_lut[128] = {
 96,   73,  55,  39,  23,  13,   5,  1,  255, 255, 255, 255, 255, 255, 255, 255,
 101,  78,  58,  42,  26,  16,   8,  2,    0,   3,  9,   17,  27,  43,  59,  79,
 102,  86,  62,  46,  32,  20,  10,  6,    4,   7,  11,  21,  33,  47,  63,  87,
 105,  90,  70,  52,  37,  28,  18,  14,  12,  15,  19,  29,  38,  53,  71,  91,
 110,  99,  82,  66,  48,  35,  30,  24,  22,  25,  31,  36,  49,  67,  83, 100,
 115, 108,  94,  76,  64,  50,  44,  40,  34,  41,  45,  51,  65,  77,  95, 109,
 118, 113, 103,  92,  80,  68,  60,  56,  54,  57,  61,  69,  81,  93, 104, 114,
 119, 116, 111, 106,  97,  88,  84,  74,  72,  75,  85,  89,  98, 107, 112, 117
};

static int DistanceToPlaneCode(int xsize, int dist) {
  const int yoffset = dist / xsize;
  const int xoffset = dist - yoffset * xsize;
  if (xoffset <= 8 && yoffset < 8) {
    return plane_to_code_lut[yoffset * 16 + 8 - xoffset] + 1;
  } else if (xoffset > xsize - 8 && yoffset < 7) {
    return plane_to_code_lut[(yoffset + 1) * 16 + 8 + (xsize - xoffset)] + 1;
  }
  return dist + 120;
}

// Returns the exact index where array1 and array2 are different. For an index
// inferior or equal to best_len_match, the return value just has to be strictly
// inferior to best_len_match. The current behavior is to return 0 if this index
// is best_len_match, and the index itself otherwise.
// If no two elements are the same, it returns max_limit.
static WEBP_INLINE int FindMatchLength(const uint32_t* const array1,
                                       const uint32_t* const array2,
                                       int best_len_match, int max_limit) {
  // Before 'expensive' linear match, check if the two arrays match at the
  // current best length index.
  if (array1[best_len_match] != array2[best_len_match]) return 0;

  return VP8LVectorMismatch(array1, array2, max_limit);
}

// -----------------------------------------------------------------------------
//  VP8LBackwardRefs

struct PixOrCopyBlock {
  PixOrCopyBlock* next_;   // next block (or NULL)
  PixOrCopy* start_;       // data start
  int size_;               // currently used size
};

static void ClearBackwardRefs(VP8LBackwardRefs* const refs) {
  assert(refs != NULL);
  if (refs->tail_ != NULL) {
    *refs->tail_ = refs->free_blocks_;  // recycle all blocks at once
  }
  refs->free_blocks_ = refs->refs_;
  refs->tail_ = &refs->refs_;
  refs->last_block_ = NULL;
  refs->refs_ = NULL;
}

void VP8LBackwardRefsClear(VP8LBackwardRefs* const refs) {
  assert(refs != NULL);
  ClearBackwardRefs(refs);
  while (refs->free_blocks_ != NULL) {
    PixOrCopyBlock* const next = refs->free_blocks_->next_;
    WebPSafeFree(refs->free_blocks_);
    refs->free_blocks_ = next;
  }
}

void VP8LBackwardRefsInit(VP8LBackwardRefs* const refs, int block_size) {
  assert(refs != NULL);
  memset(refs, 0, sizeof(*refs));
  refs->tail_ = &refs->refs_;
  refs->block_size_ =
      (block_size < MIN_BLOCK_SIZE) ? MIN_BLOCK_SIZE : block_size;
}

VP8LRefsCursor VP8LRefsCursorInit(const VP8LBackwardRefs* const refs) {
  VP8LRefsCursor c;
  c.cur_block_ = refs->refs_;
  if (refs->refs_ != NULL) {
    c.cur_pos = c.cur_block_->start_;
    c.last_pos_ = c.cur_pos + c.cur_block_->size_;
  } else {
    c.cur_pos = NULL;
    c.last_pos_ = NULL;
  }
  return c;
}

void VP8LRefsCursorNextBlock(VP8LRefsCursor* const c) {
  PixOrCopyBlock* const b = c->cur_block_->next_;
  c->cur_pos = (b == NULL) ? NULL : b->start_;
  c->last_pos_ = (b == NULL) ? NULL : b->start_ + b->size_;
  c->cur_block_ = b;
}

// Create a new block, either from the free list or allocated
static PixOrCopyBlock* BackwardRefsNewBlock(VP8LBackwardRefs* const refs) {
  PixOrCopyBlock* b = refs->free_blocks_;
  if (b == NULL) {   // allocate new memory chunk
    const size_t total_size =
        sizeof(*b) + refs->block_size_ * sizeof(*b->start_);
    b = (PixOrCopyBlock*)WebPSafeMalloc(1ULL, total_size);
    if (b == NULL) {
      refs->error_ |= 1;
      return NULL;
    }
    b->start_ = (PixOrCopy*)((uint8_t*)b + sizeof(*b));  // not always aligned
  } else {  // recycle from free-list
    refs->free_blocks_ = b->next_;
  }
  *refs->tail_ = b;
  refs->tail_ = &b->next_;
  refs->last_block_ = b;
  b->next_ = NULL;
  b->size_ = 0;
  return b;
}

static WEBP_INLINE void BackwardRefsCursorAdd(VP8LBackwardRefs* const refs,
                                              const PixOrCopy v) {
  PixOrCopyBlock* b = refs->last_block_;
  if (b == NULL || b->size_ == refs->block_size_) {
    b = BackwardRefsNewBlock(refs);
    if (b == NULL) return;   // refs->error_ is set
  }
  b->start_[b->size_++] = v;
}

// -----------------------------------------------------------------------------
// Hash chains

int VP8LHashChainInit(VP8LHashChain* const p, int size) {
  assert(p->size_ == 0);
  assert(p->offset_length_ == NULL);
  assert(size > 0);
  p->offset_length_ =
      (uint32_t*)WebPSafeMalloc(size, sizeof(*p->offset_length_));
  if (p->offset_length_ == NULL) return 0;
  p->size_ = size;

  return 1;
}

void VP8LHashChainClear(VP8LHashChain* const p) {
  assert(p != NULL);
  WebPSafeFree(p->offset_length_);

  p->size_ = 0;
  p->offset_length_ = NULL;
}

// -----------------------------------------------------------------------------

#define HASH_MULTIPLIER_HI (0xc6a4a793ULL)
#define HASH_MULTIPLIER_LO (0x5bd1e996ULL)

static WEBP_INLINE uint32_t GetPixPairHash64(const uint32_t* const argb) {
  uint32_t key;
  key  = (argb[1] * HASH_MULTIPLIER_HI) & 0xffffffffu;
  key += (argb[0] * HASH_MULTIPLIER_LO) & 0xffffffffu;
  key = key >> (32 - HASH_BITS);
  return key;
}

// Returns the maximum number of hash chain lookups to do for a
// given compression quality. Return value in range [8, 86].
static int GetMaxItersForQuality(int quality) {
  return 8 + (quality * quality) / 128;
}

static int GetWindowSizeForHashChain(int quality, int xsize) {
  const int max_window_size = (quality > 75) ? WINDOW_SIZE
                            : (quality > 50) ? (xsize << 8)
                            : (quality > 25) ? (xsize << 6)
                            : (xsize << 4);
  assert(xsize > 0);
  return (max_window_size > WINDOW_SIZE) ? WINDOW_SIZE : max_window_size;
}

static WEBP_INLINE int MaxFindCopyLength(int len) {
  return (len < MAX_LENGTH) ? len : MAX_LENGTH;
}

int VP8LHashChainFill(VP8LHashChain* const p, int quality,
                      const uint32_t* const argb, int xsize, int ysize,
                      int low_effort) {
  const int size = xsize * ysize;
  const int iter_max = GetMaxItersForQuality(quality);
  const uint32_t window_size = GetWindowSizeForHashChain(quality, xsize);
  int pos;
  int argb_comp;
  uint32_t base_position;
  int32_t* hash_to_first_index;
  // Temporarily use the p->offset_length_ as a hash chain.
  int32_t* chain = (int32_t*)p->offset_length_;
  assert(size > 0);
  assert(p->size_ != 0);
  assert(p->offset_length_ != NULL);

  if (size <= 2) {
    p->offset_length_[0] = p->offset_length_[size - 1] = 0;
    return 1;
  }

  hash_to_first_index =
      (int32_t*)WebPSafeMalloc(HASH_SIZE, sizeof(*hash_to_first_index));
  if (hash_to_first_index == NULL) return 0;

  // Set the int32_t array to -1.
  memset(hash_to_first_index, 0xff, HASH_SIZE * sizeof(*hash_to_first_index));
  // Fill the chain linking pixels with the same hash.
  argb_comp = (argb[0] == argb[1]);
  for (pos = 0; pos < size - 2;) {
    uint32_t hash_code;
    const int argb_comp_next = (argb[pos + 1] == argb[pos + 2]);
    if (argb_comp && argb_comp_next) {
      // Consecutive pixels with the same color will share the same hash.
      // We therefore use a different hash: the color and its repetition
      // length.
      uint32_t tmp[2];
      uint32_t len = 1;
      tmp[0] = argb[pos];
      // Figure out how far the pixels are the same.
      // The last pixel has a different 64 bit hash, as its next pixel does
      // not have the same color, so we just need to get to the last pixel equal
      // to its follower.
      while (pos + (int)len + 2 < size && argb[pos + len + 2] == argb[pos]) {
        ++len;
      }
      if (len > MAX_LENGTH) {
        // Skip the pixels that match for distance=1 and length>MAX_LENGTH
        // because they are linked to their predecessor and we automatically
        // check that in the main for loop below. Skipping means setting no
        // predecessor in the chain, hence -1.
        memset(chain + pos, 0xff, (len - MAX_LENGTH) * sizeof(*chain));
        pos += len - MAX_LENGTH;
        len = MAX_LENGTH;
      }
      // Process the rest of the hash chain.
      while (len) {
        tmp[1] = len--;
        hash_code = GetPixPairHash64(tmp);
        chain[pos] = hash_to_first_index[hash_code];
        hash_to_first_index[hash_code] = pos++;
      }
      argb_comp = 0;
    } else {
      // Just move one pixel forward.
      hash_code = GetPixPairHash64(argb + pos);
      chain[pos] = hash_to_first_index[hash_code];
      hash_to_first_index[hash_code] = pos++;
      argb_comp = argb_comp_next;
    }
  }
  // Process the penultimate pixel.
  chain[pos] = hash_to_first_index[GetPixPairHash64(argb + pos)];

  WebPSafeFree(hash_to_first_index);

  // Find the best match interval at each pixel, defined by an offset to the
  // pixel and a length. The right-most pixel cannot match anything to the right
  // (hence a best length of 0) and the left-most pixel nothing to the left
  // (hence an offset of 0).
  assert(size > 2);
  p->offset_length_[0] = p->offset_length_[size - 1] = 0;
  for (base_position = size - 2; base_position > 0;) {
    const int max_len = MaxFindCopyLength(size - 1 - base_position);
    const uint32_t* const argb_start = argb + base_position;
    int iter = iter_max;
    int best_length = 0;
    uint32_t best_distance = 0;
    uint32_t best_argb;
    const int min_pos =
        (base_position > window_size) ? base_position - window_size : 0;
    const int length_max = (max_len < 256) ? max_len : 256;
    uint32_t max_base_position;

    pos = chain[base_position];
    if (!low_effort) {
      int curr_length;
      // Heuristic: use the comparison with the above line as an initialization.
      if (base_position >= (uint32_t)xsize) {
        curr_length = FindMatchLength(argb_start - xsize, argb_start,
                                      best_length, max_len);
        if (curr_length > best_length) {
          best_length = curr_length;
          best_distance = xsize;
        }
        --iter;
      }
      // Heuristic: compare to the previous pixel.
      curr_length =
          FindMatchLength(argb_start - 1, argb_start, best_length, max_len);
      if (curr_length > best_length) {
        best_length = curr_length;
        best_distance = 1;
      }
      --iter;
      // Skip the for loop if we already have the maximum.
      if (best_length == MAX_LENGTH) pos = min_pos - 1;
    }
    best_argb = argb_start[best_length];

    for (; pos >= min_pos && --iter; pos = chain[pos]) {
      int curr_length;
      assert(base_position > (uint32_t)pos);

      if (argb[pos + best_length] != best_argb) continue;

      curr_length = VP8LVectorMismatch(argb + pos, argb_start, max_len);
      if (best_length < curr_length) {
        best_length = curr_length;
        best_distance = base_position - pos;
        best_argb = argb_start[best_length];
        // Stop if we have reached a good enough length.
        if (best_length >= length_max) break;
      }
    }
    // We have the best match but in case the two intervals continue matching
    // to the left, we have the best matches for the left-extended pixels.
    max_base_position = base_position;
    while (1) {
      assert(best_length <= MAX_LENGTH);
      assert(best_distance <= WINDOW_SIZE);
      p->offset_length_[base_position] =
          (best_distance << MAX_LENGTH_BITS) | (uint32_t)best_length;
      --base_position;
      // Stop if we don't have a match or if we are out of bounds.
      if (best_distance == 0 || base_position == 0) break;
      // Stop if we cannot extend the matching intervals to the left.
      if (base_position < best_distance ||
          argb[base_position - best_distance] != argb[base_position]) {
        break;
      }
      // Stop if we are matching at its limit because there could be a closer
      // matching interval with the same maximum length. Then again, if the
      // matching interval is as close as possible (best_distance == 1), we will
      // never find anything better so let's continue.
      if (best_length == MAX_LENGTH && best_distance != 1 &&
          base_position + MAX_LENGTH < max_base_position) {
        break;
      }
      if (best_length < MAX_LENGTH) {
        ++best_length;
        max_base_position = base_position;
      }
    }
  }
  return 1;
}

static WEBP_INLINE int HashChainFindOffset(const VP8LHashChain* const p,
                                           const int base_position) {
  return p->offset_length_[base_position] >> MAX_LENGTH_BITS;
}

static WEBP_INLINE int HashChainFindLength(const VP8LHashChain* const p,
                                           const int base_position) {
  return p->offset_length_[base_position] & ((1U << MAX_LENGTH_BITS) - 1);
}

static WEBP_INLINE void HashChainFindCopy(const VP8LHashChain* const p,
                                          int base_position,
                                          int* const offset_ptr,
                                          int* const length_ptr) {
  *offset_ptr = HashChainFindOffset(p, base_position);
  *length_ptr = HashChainFindLength(p, base_position);
}

static WEBP_INLINE void AddSingleLiteral(uint32_t pixel, int use_color_cache,
                                         VP8LColorCache* const hashers,
                                         VP8LBackwardRefs* const refs) {
  PixOrCopy v;
  if (use_color_cache) {
    const uint32_t key = VP8LColorCacheGetIndex(hashers, pixel);
    if (VP8LColorCacheLookup(hashers, key) == pixel) {
      v = PixOrCopyCreateCacheIdx(key);
    } else {
      v = PixOrCopyCreateLiteral(pixel);
      VP8LColorCacheSet(hashers, key, pixel);
    }
  } else {
    v = PixOrCopyCreateLiteral(pixel);
  }
  BackwardRefsCursorAdd(refs, v);
}

static int BackwardReferencesRle(int xsize, int ysize,
                                 const uint32_t* const argb,
                                 int cache_bits, VP8LBackwardRefs* const refs) {
  const int pix_count = xsize * ysize;
  int i, k;
  const int use_color_cache = (cache_bits > 0);
  VP8LColorCache hashers;

  if (use_color_cache && !VP8LColorCacheInit(&hashers, cache_bits)) {
    return 0;
  }
  ClearBackwardRefs(refs);
  // Add first pixel as literal.
  AddSingleLiteral(argb[0], use_color_cache, &hashers, refs);
  i = 1;
  while (i < pix_count) {
    const int max_len = MaxFindCopyLength(pix_count - i);
    const int rle_len = FindMatchLength(argb + i, argb + i - 1, 0, max_len);
    const int prev_row_len = (i < xsize) ? 0 :
        FindMatchLength(argb + i, argb + i - xsize, 0, max_len);
    if (rle_len >= prev_row_len && rle_len >= MIN_LENGTH) {
      BackwardRefsCursorAdd(refs, PixOrCopyCreateCopy(1, rle_len));
      // We don't need to update the color cache here since it is always the
      // same pixel being copied, and that does not change the color cache
      // state.
      i += rle_len;
    } else if (prev_row_len >= MIN_LENGTH) {
      BackwardRefsCursorAdd(refs, PixOrCopyCreateCopy(xsize, prev_row_len));
      if (use_color_cache) {
        for (k = 0; k < prev_row_len; ++k) {
          VP8LColorCacheInsert(&hashers, argb[i + k]);
        }
      }
      i += prev_row_len;
    } else {
      AddSingleLiteral(argb[i], use_color_cache, &hashers, refs);
      i++;
    }
  }
  if (use_color_cache) VP8LColorCacheClear(&hashers);
  return !refs->error_;
}

static int BackwardReferencesLz77(int xsize, int ysize,
                                  const uint32_t* const argb, int cache_bits,
                                  const VP8LHashChain* const hash_chain,
                                  VP8LBackwardRefs* const refs) {
  int i;
  int i_last_check = -1;
  int ok = 0;
  int cc_init = 0;
  const int use_color_cache = (cache_bits > 0);
  const int pix_count = xsize * ysize;
  VP8LColorCache hashers;

  if (use_color_cache) {
    cc_init = VP8LColorCacheInit(&hashers, cache_bits);
    if (!cc_init) goto Error;
  }
  ClearBackwardRefs(refs);
  for (i = 0; i < pix_count;) {
    // Alternative#1: Code the pixels starting at 'i' using backward reference.
    int offset = 0;
    int len = 0;
    int j;
    HashChainFindCopy(hash_chain, i, &offset, &len);
    if (len >= MIN_LENGTH) {
      const int len_ini = len;
      int max_reach = 0;
      assert(i + len < pix_count);
      // Only start from what we have not checked already.
      i_last_check = (i > i_last_check) ? i : i_last_check;
      // We know the best match for the current pixel but we try to find the
      // best matches for the current pixel AND the next one combined.
      // The naive method would use the intervals:
      // [i,i+len) + [i+len, length of best match at i+len)
      // while we check if we can use:
      // [i,j) (where j<=i+len) + [j, length of best match at j)
      for (j = i_last_check + 1; j <= i + len_ini; ++j) {
        const int len_j = HashChainFindLength(hash_chain, j);
        const int reach =
            j + (len_j >= MIN_LENGTH ? len_j : 1);  // 1 for single literal.
        if (reach > max_reach) {
          len = j - i;
          max_reach = reach;
        }
      }
    } else {
      len = 1;
    }
    // Go with literal or backward reference.
    assert(len > 0);
    if (len == 1) {
      AddSingleLiteral(argb[i], use_color_cache, &hashers, refs);
    } else {
      BackwardRefsCursorAdd(refs, PixOrCopyCreateCopy(offset, len));
      if (use_color_cache) {
        for (j = i; j < i + len; ++j) VP8LColorCacheInsert(&hashers, argb[j]);
      }
    }
    i += len;
  }

  ok = !refs->error_;
 Error:
  if (cc_init) VP8LColorCacheClear(&hashers);
  return ok;
}

// -----------------------------------------------------------------------------

typedef struct {
  double alpha_[VALUES_IN_BYTE];
  double red_[VALUES_IN_BYTE];
  double blue_[VALUES_IN_BYTE];
  double distance_[NUM_DISTANCE_CODES];
  double* literal_;
} CostModel;

static void ConvertPopulationCountTableToBitEstimates(
    int num_symbols, const uint32_t population_counts[], double output[]) {
  uint32_t sum = 0;
  int nonzeros = 0;
  int i;
  for (i = 0; i < num_symbols; ++i) {
    sum += population_counts[i];
    if (population_counts[i] > 0) {
      ++nonzeros;
    }
  }
  if (nonzeros <= 1) {
    memset(output, 0, num_symbols * sizeof(*output));
  } else {
    const double logsum = VP8LFastLog2(sum);
    for (i = 0; i < num_symbols; ++i) {
      output[i] = logsum - VP8LFastLog2(population_counts[i]);
    }
  }
}

static int CostModelBuild(CostModel* const m, int cache_bits,
                          const VP8LBackwardRefs* const refs) {
  int ok = 0;
  VP8LHistogram* const histo = VP8LAllocateHistogram(cache_bits);
  if (histo == NULL) goto Error;

  VP8LHistogramCreate(histo, refs, cache_bits);

  ConvertPopulationCountTableToBitEstimates(
      VP8LHistogramNumCodes(histo->palette_code_bits_),
      histo->literal_, m->literal_);
  ConvertPopulationCountTableToBitEstimates(
      VALUES_IN_BYTE, histo->red_, m->red_);
  ConvertPopulationCountTableToBitEstimates(
      VALUES_IN_BYTE, histo->blue_, m->blue_);
  ConvertPopulationCountTableToBitEstimates(
      VALUES_IN_BYTE, histo->alpha_, m->alpha_);
  ConvertPopulationCountTableToBitEstimates(
      NUM_DISTANCE_CODES, histo->distance_, m->distance_);
  ok = 1;

 Error:
  VP8LFreeHistogram(histo);
  return ok;
}

static WEBP_INLINE double GetLiteralCost(const CostModel* const m, uint32_t v) {
  return m->alpha_[v >> 24] +
         m->red_[(v >> 16) & 0xff] +
         m->literal_[(v >> 8) & 0xff] +
         m->blue_[v & 0xff];
}

static WEBP_INLINE double GetCacheCost(const CostModel* const m, uint32_t idx) {
  const int literal_idx = VALUES_IN_BYTE + NUM_LENGTH_CODES + idx;
  return m->literal_[literal_idx];
}

static WEBP_INLINE double GetLengthCost(const CostModel* const m,
                                        uint32_t length) {
  int code, extra_bits;
  VP8LPrefixEncodeBits(length, &code, &extra_bits);
  return m->literal_[VALUES_IN_BYTE + code] + extra_bits;
}

static WEBP_INLINE double GetDistanceCost(const CostModel* const m,
                                          uint32_t distance) {
  int code, extra_bits;
  VP8LPrefixEncodeBits(distance, &code, &extra_bits);
  return m->distance_[code] + extra_bits;
}

static WEBP_INLINE void AddSingleLiteralWithCostModel(
    const uint32_t* const argb, VP8LColorCache* const hashers,
    const CostModel* const cost_model, int idx, int use_color_cache,
    float prev_cost, float* const cost, uint16_t* const dist_array) {
  double cost_val = prev_cost;
  const uint32_t color = argb[0];
  const int ix = use_color_cache ? VP8LColorCacheContains(hashers, color) : -1;
  if (ix >= 0) {
    // use_color_cache is true and hashers contains color
    const double mul0 = 0.68;
    cost_val += GetCacheCost(cost_model, ix) * mul0;
  } else {
    const double mul1 = 0.82;
    if (use_color_cache) VP8LColorCacheInsert(hashers, color);
    cost_val += GetLiteralCost(cost_model, color) * mul1;
  }
  if (cost[idx] > cost_val) {
    cost[idx] = (float)cost_val;
    dist_array[idx] = 1;  // only one is inserted.
  }
}

// -----------------------------------------------------------------------------
// CostManager and interval handling

// Empirical value to avoid high memory consumption but good for performance.
#define COST_CACHE_INTERVAL_SIZE_MAX 500

// To perform backward reference every pixel at index index_ is considered and
// the cost for the MAX_LENGTH following pixels computed. Those following pixels
// at index index_ + k (k from 0 to MAX_LENGTH) have a cost of:
//     cost_ = distance cost at index + GetLengthCost(cost_model, k)
// and the minimum value is kept. GetLengthCost(cost_model, k) is cached in an
// array of size MAX_LENGTH.
// Instead of performing MAX_LENGTH comparisons per pixel, we keep track of the
// minimal values using intervals of constant cost.
// An interval is defined by the index_ of the pixel that generated it and
// is only useful in a range of indices from start_ to end_ (exclusive), i.e.
// it contains the minimum value for pixels between start_ and end_.
// Intervals are stored in a linked list and ordered by start_. When a new
// interval has a better value, old intervals are split or removed. There are
// therefore no overlapping intervals.
typedef struct CostInterval CostInterval;
struct CostInterval {
  float cost_;
  int start_;
  int end_;
  int index_;
  CostInterval* previous_;
  CostInterval* next_;
};

// The GetLengthCost(cost_model, k) are cached in a CostCacheInterval.
typedef struct {
  double cost_;
  int start_;
  int end_;       // Exclusive.
} CostCacheInterval;

// This structure is in charge of managing intervals and costs.
// It caches the different CostCacheInterval, caches the different
// GetLengthCost(cost_model, k) in cost_cache_ and the CostInterval's (whose
// count_ is limited by COST_CACHE_INTERVAL_SIZE_MAX).
#define COST_MANAGER_MAX_FREE_LIST 10
typedef struct {
  CostInterval* head_;
  int count_;  // The number of stored intervals.
  CostCacheInterval* cache_intervals_;
  size_t cache_intervals_size_;
  double cost_cache_[MAX_LENGTH];  // Contains the GetLengthCost(cost_model, k).
  float* costs_;
  uint16_t* dist_array_;
  // Most of the time, we only need few intervals -> use a free-list, to avoid
  // fragmentation with small allocs in most common cases.
  CostInterval intervals_[COST_MANAGER_MAX_FREE_LIST];
  CostInterval* free_intervals_;
  // These are regularly malloc'd remains. This list can't grow larger than than
  // size COST_CACHE_INTERVAL_SIZE_MAX - COST_MANAGER_MAX_FREE_LIST, note.
  CostInterval* recycled_intervals_;
} CostManager;

static void CostIntervalAddToFreeList(CostManager* const manager,
                                      CostInterval* const interval) {
  interval->next_ = manager->free_intervals_;
  manager->free_intervals_ = interval;
}

static int CostIntervalIsInFreeList(const CostManager* const manager,
                                    const CostInterval* const interval) {
  return (interval >= &manager->intervals_[0] &&
          interval <= &manager->intervals_[COST_MANAGER_MAX_FREE_LIST - 1]);
}

static void CostManagerInitFreeList(CostManager* const manager) {
  int i;
  manager->free_intervals_ = NULL;
  for (i = 0; i < COST_MANAGER_MAX_FREE_LIST; ++i) {
    CostIntervalAddToFreeList(manager, &manager->intervals_[i]);
  }
}

static void DeleteIntervalList(CostManager* const manager,
                               const CostInterval* interval) {
  while (interval != NULL) {
    const CostInterval* const next = interval->next_;
    if (!CostIntervalIsInFreeList(manager, interval)) {
      WebPSafeFree((void*)interval);
    }  // else: do nothing
    interval = next;
  }
}

static void CostManagerClear(CostManager* const manager) {
  if (manager == NULL) return;

  WebPSafeFree(manager->costs_);
  WebPSafeFree(manager->cache_intervals_);

  // Clear the interval lists.
  DeleteIntervalList(manager, manager->head_);
  manager->head_ = NULL;
  DeleteIntervalList(manager, manager->recycled_intervals_);
  manager->recycled_intervals_ = NULL;

  // Reset pointers, count_ and cache_intervals_size_.
  memset(manager, 0, sizeof(*manager));
  CostManagerInitFreeList(manager);
}

static int CostManagerInit(CostManager* const manager,
                           uint16_t* const dist_array, int pix_count,
                           const CostModel* const cost_model) {
  int i;
  const int cost_cache_size = (pix_count > MAX_LENGTH) ? MAX_LENGTH : pix_count;

  manager->costs_ = NULL;
  manager->cache_intervals_ = NULL;
  manager->head_ = NULL;
  manager->recycled_intervals_ = NULL;
  manager->count_ = 0;
  manager->dist_array_ = dist_array;
  CostManagerInitFreeList(manager);

  // Fill in the cost_cache_.
  manager->cache_intervals_size_ = 1;
  manager->cost_cache_[0] = GetLengthCost(cost_model, 0);
  for (i = 1; i < cost_cache_size; ++i) {
    manager->cost_cache_[i] = GetLengthCost(cost_model, i);
    // Get an approximation of the number of bound intervals.
    if (manager->cost_cache_[i] != manager->cost_cache_[i - 1]) {
      ++manager->cache_intervals_size_;
    }
  }

  // With the current cost model, we usually have below 20 intervals.
  // The worst case scenario with a cost model would be if every length has a
  // different cost, hence MAX_LENGTH but that is impossible with the current
  // implementation that spirals around a pixel.
  assert(manager->cache_intervals_size_ <= MAX_LENGTH);
  manager->cache_intervals_ = (CostCacheInterval*)WebPSafeMalloc(
      manager->cache_intervals_size_, sizeof(*manager->cache_intervals_));
  if (manager->cache_intervals_ == NULL) {
    CostManagerClear(manager);
    return 0;
  }

  // Fill in the cache_intervals_.
  {
    CostCacheInterval* cur = manager->cache_intervals_;

    // Consecutive values in cost_cache_ are compared and if a big enough
    // difference is found, a new interval is created and bounded.
    cur->start_ = 0;
    cur->end_ = 1;
    cur->cost_ = manager->cost_cache_[0];
    for (i = 1; i < cost_cache_size; ++i) {
      const double cost_val = manager->cost_cache_[i];
      if (cost_val != cur->cost_) {
        ++cur;
        // Initialize an interval.
        cur->start_ = i;
        cur->cost_ = cost_val;
      }
      cur->end_ = i + 1;
    }
    manager->cache_intervals_size_ = cur + 1 - manager->cache_intervals_;
  }

  manager->costs_ = (float*)WebPSafeMalloc(pix_count, sizeof(*manager->costs_));
  if (manager->costs_ == NULL) {
    CostManagerClear(manager);
    return 0;
  }
  // Set the initial costs_ high for every pixel as we will keep the minimum.
  for (i = 0; i < pix_count; ++i) manager->costs_[i] = 1e38f;

  return 1;
}

// Given the cost and the position that define an interval, update the cost at
// pixel 'i' if it is smaller than the previously computed value.
static WEBP_INLINE void UpdateCost(CostManager* const manager, int i,
                                   int position, float cost) {
  const int k = i - position;
  assert(k >= 0 && k < MAX_LENGTH);

  if (manager->costs_[i] > cost) {
    manager->costs_[i] = cost;
    manager->dist_array_[i] = k + 1;
  }
}

// Given the cost and the position that define an interval, update the cost for
// all the pixels between 'start' and 'end' excluded.
static WEBP_INLINE void UpdateCostPerInterval(CostManager* const manager,
                                              int start, int end, int position,
                                              float cost) {
  int i;
  for (i = start; i < end; ++i) UpdateCost(manager, i, position, cost);
}

// Given two intervals, make 'prev' be the previous one of 'next' in 'manager'.
static WEBP_INLINE void ConnectIntervals(CostManager* const manager,
                                         CostInterval* const prev,
                                         CostInterval* const next) {
  if (prev != NULL) {
    prev->next_ = next;
  } else {
    manager->head_ = next;
  }

  if (next != NULL) next->previous_ = prev;
}

// Pop an interval in the manager.
static WEBP_INLINE void PopInterval(CostManager* const manager,
                                    CostInterval* const interval) {
  if (interval == NULL) return;

  ConnectIntervals(manager, interval->previous_, interval->next_);
  if (CostIntervalIsInFreeList(manager, interval)) {
    CostIntervalAddToFreeList(manager, interval);
  } else {  // recycle regularly malloc'd intervals too
    interval->next_ = manager->recycled_intervals_;
    manager->recycled_intervals_ = interval;
  }
  --manager->count_;
  assert(manager->count_ >= 0);
}

// Update the cost at index i by going over all the stored intervals that
// overlap with i.
// If 'do_clean_intervals' is set to something different than 0, intervals that
// end before 'i' will be popped.
static WEBP_INLINE void UpdateCostAtIndex(CostManager* const manager, int i,
                                          int do_clean_intervals) {
  CostInterval* current = manager->head_;

  while (current != NULL && current->start_ <= i) {
    CostInterval* const next = current->next_;
    if (current->end_ <= i) {
      if (do_clean_intervals) {
        // We have an outdated interval, remove it.
        PopInterval(manager, current);
      }
    } else {
      UpdateCost(manager, i, current->index_, current->cost_);
    }
    current = next;
  }
}

// Given a current orphan interval and its previous interval, before
// it was orphaned (which can be NULL), set it at the right place in the list
// of intervals using the start_ ordering and the previous interval as a hint.
static WEBP_INLINE void PositionOrphanInterval(CostManager* const manager,
                                               CostInterval* const current,
                                               CostInterval* previous) {
  assert(current != NULL);

  if (previous == NULL) previous = manager->head_;
  while (previous != NULL && current->start_ < previous->start_) {
    previous = previous->previous_;
  }
  while (previous != NULL && previous->next_ != NULL &&
         previous->next_->start_ < current->start_) {
    previous = previous->next_;
  }

  if (previous != NULL) {
    ConnectIntervals(manager, current, previous->next_);
  } else {
    ConnectIntervals(manager, current, manager->head_);
  }
  ConnectIntervals(manager, previous, current);
}

// Insert an interval in the list contained in the manager by starting at
// interval_in as a hint. The intervals are sorted by start_ value.
static WEBP_INLINE void InsertInterval(CostManager* const manager,
                                       CostInterval* const interval_in,
                                       float cost, int position, int start,
                                       int end) {
  CostInterval* interval_new;

  if (start >= end) return;
  if (manager->count_ >= COST_CACHE_INTERVAL_SIZE_MAX) {
    // Serialize the interval if we cannot store it.
    UpdateCostPerInterval(manager, start, end, position, cost);
    return;
  }
  if (manager->free_intervals_ != NULL) {
    interval_new = manager->free_intervals_;
    manager->free_intervals_ = interval_new->next_;
  } else if (manager->recycled_intervals_ != NULL) {
    interval_new = manager->recycled_intervals_;
    manager->recycled_intervals_ = interval_new->next_;
  } else {  // malloc for good
    interval_new = (CostInterval*)WebPSafeMalloc(1, sizeof(*interval_new));
    if (interval_new == NULL) {
      // Write down the interval if we cannot create it.
      UpdateCostPerInterval(manager, start, end, position, cost);
      return;
    }
  }

  interval_new->cost_ = cost;
  interval_new->index_ = position;
  interval_new->start_ = start;
  interval_new->end_ = end;
  PositionOrphanInterval(manager, interval_new, interval_in);

  ++manager->count_;
}

// Given a new cost interval defined by its start at position, its length value
// and distance_cost, add its contributions to the previous intervals and costs.
// If handling the interval or one of its subintervals becomes to heavy, its
// contribution is added to the costs right away.
static WEBP_INLINE void PushInterval(CostManager* const manager,
                                     double distance_cost, int position,
                                     int len) {
  size_t i;
  CostInterval* interval = manager->head_;
  CostInterval* interval_next;
  const CostCacheInterval* const cost_cache_intervals =
      manager->cache_intervals_;
  // If the interval is small enough, no need to deal with the heavy
  // interval logic, just serialize it right away. This constant is empirical.
  const int kSkipDistance = 10;

  if (len < kSkipDistance) {
    int j;
    for (j = position; j < position + len; ++j) {
      const int k = j - position;
      float cost_tmp;
      assert(k >= 0 && k < MAX_LENGTH);
      cost_tmp = (float)(distance_cost + manager->cost_cache_[k]);

      if (manager->costs_[j] > cost_tmp) {
        manager->costs_[j] = cost_tmp;
        manager->dist_array_[j] = k + 1;
      }
    }
    return;
  }

  for (i = 0; i < manager->cache_intervals_size_ &&
              cost_cache_intervals[i].start_ < len;
       ++i) {
    // Define the intersection of the ith interval with the new one.
    int start = position + cost_cache_intervals[i].start_;
    const int end = position + (cost_cache_intervals[i].end_ > len
                                 ? len
                                 : cost_cache_intervals[i].end_);
    const float cost = (float)(distance_cost + cost_cache_intervals[i].cost_);

    for (; interval != NULL && interval->start_ < end;
         interval = interval_next) {
      interval_next = interval->next_;

      // Make sure we have some overlap
      if (start >= interval->end_) continue;

      if (cost >= interval->cost_) {
        // When intervals are represented, the lower, the better.
        // [**********************************************************[
        // start                                                    end
        //                   [----------------------------------[
        //                   interval->start_       interval->end_
        // If we are worse than what we already have, add whatever we have so
        // far up to interval.
        const int start_new = interval->end_;
        InsertInterval(manager, interval, cost, position, start,
                       interval->start_);
        start = start_new;
        if (start >= end) break;
        continue;
      }

      if (start <= interval->start_) {
        if (interval->end_ <= end) {
          //                   [----------------------------------[
          //                   interval->start_       interval->end_
          // [**************************************************************[
          // start                                                        end
          // We can safely remove the old interval as it is fully included.
          PopInterval(manager, interval);
        } else {
          //              [------------------------------------[
          //              interval->start_        interval->end_
          // [*****************************[
          // start                       end
          interval->start_ = end;
          break;
        }
      } else {
        if (end < interval->end_) {
          // [--------------------------------------------------------------[
          // interval->start_                                  interval->end_
          //                     [*****************************[
          //                     start                       end
          // We have to split the old interval as it fully contains the new one.
          const int end_original = interval->end_;
          interval->end_ = start;
          InsertInterval(manager, interval, interval->cost_, interval->index_,
                         end, end_original);
          interval = interval->next_;
          break;
        } else {
          // [------------------------------------[
          // interval->start_        interval->end_
          //                     [*****************************[
          //                     start                       end
          interval->end_ = start;
        }
      }
    }
    // Insert the remaining interval from start to end.
    InsertInterval(manager, interval, cost, position, start, end);
  }
}

static int BackwardReferencesHashChainDistanceOnly(
    int xsize, int ysize, const uint32_t* const argb, int cache_bits,
    const VP8LHashChain* const hash_chain, const VP8LBackwardRefs* const refs,
    uint16_t* const dist_array) {
  int i;
  int ok = 0;
  int cc_init = 0;
  const int pix_count = xsize * ysize;
  const int use_color_cache = (cache_bits > 0);
  const size_t literal_array_size =
      sizeof(double) * (NUM_LITERAL_CODES + NUM_LENGTH_CODES +
                        ((cache_bits > 0) ? (1 << cache_bits) : 0));
  const size_t cost_model_size = sizeof(CostModel) + literal_array_size;
  CostModel* const cost_model =
      (CostModel*)WebPSafeCalloc(1ULL, cost_model_size);
  VP8LColorCache hashers;
  CostManager* cost_manager =
      (CostManager*)WebPSafeMalloc(1ULL, sizeof(*cost_manager));
  int offset_prev = -1, len_prev = -1;
  double offset_cost = -1;
  int first_offset_is_constant = -1;  // initialized with 'impossible' value
  int reach = 0;

  if (cost_model == NULL || cost_manager == NULL) goto Error;

  cost_model->literal_ = (double*)(cost_model + 1);
  if (use_color_cache) {
    cc_init = VP8LColorCacheInit(&hashers, cache_bits);
    if (!cc_init) goto Error;
  }

  if (!CostModelBuild(cost_model, cache_bits, refs)) {
    goto Error;
  }

  if (!CostManagerInit(cost_manager, dist_array, pix_count, cost_model)) {
    goto Error;
  }

  // We loop one pixel at a time, but store all currently best points to
  // non-processed locations from this point.
  dist_array[0] = 0;
  // Add first pixel as literal.
  AddSingleLiteralWithCostModel(argb + 0, &hashers, cost_model, 0,
                                use_color_cache, 0.f, cost_manager->costs_,
                                dist_array);

  for (i = 1; i < pix_count; ++i) {
    const float prev_cost = cost_manager->costs_[i - 1];
    int offset, len;
    HashChainFindCopy(hash_chain, i, &offset, &len);

    // Try adding the pixel as a literal.
    AddSingleLiteralWithCostModel(argb + i, &hashers, cost_model, i,
                                  use_color_cache, prev_cost,
                                  cost_manager->costs_, dist_array);

    // If we are dealing with a non-literal.
    if (len >= 2) {
      if (offset != offset_prev) {
        const int code = DistanceToPlaneCode(xsize, offset);
        offset_cost = GetDistanceCost(cost_model, code);
        first_offset_is_constant = 1;
        PushInterval(cost_manager, prev_cost + offset_cost, i, len);
      } else {
        assert(offset_cost >= 0);
        assert(len_prev >= 0);
        assert(first_offset_is_constant == 0 || first_offset_is_constant == 1);
        // Instead of considering all contributions from a pixel i by calling:
        //         PushInterval(cost_manager, prev_cost + offset_cost, i, len);
        // we optimize these contributions in case offset_cost stays the same
        // for consecutive pixels. This describes a set of pixels similar to a
        // previous set (e.g. constant color regions).
        if (first_offset_is_constant) {
          reach = i - 1 + len_prev - 1;
          first_offset_is_constant = 0;
        }

        if (i + len - 1 > reach) {
          // We can only be go further with the same offset if the previous
          // length was maxed, hence len_prev == len == MAX_LENGTH.
          // TODO(vrabaud), bump i to the end right away (insert cache and
          // update cost).
          // TODO(vrabaud), check if one of the points in between does not have
          // a lower cost.
          // Already consider the pixel at "reach" to add intervals that are
          // better than whatever we add.
          int offset_j, len_j = 0;
          int j;
          assert(len == MAX_LENGTH);
          // Figure out the last consecutive pixel within [i, reach + 1] with
          // the same offset.
          for (j = i; j <= reach; ++j) {
            HashChainFindCopy(hash_chain, j + 1, &offset_j, &len_j);
            if (offset_j != offset) {
              HashChainFindCopy(hash_chain, j, &offset_j, &len_j);
              break;
            }
          }
          // Update the cost at j - 1 and j.
          UpdateCostAtIndex(cost_manager, j - 1, 0);
          UpdateCostAtIndex(cost_manager, j, 0);

          PushInterval(cost_manager, cost_manager->costs_[j - 1] + offset_cost,
                       j, len_j);
          reach = j + len_j - 1;
        }
      }
    }

    UpdateCostAtIndex(cost_manager, i, 1);
    offset_prev = offset;
    len_prev = len;
  }

  ok = !refs->error_;
Error:
  if (cc_init) VP8LColorCacheClear(&hashers);
  CostManagerClear(cost_manager);
  WebPSafeFree(cost_model);
  WebPSafeFree(cost_manager);
  return ok;
}

// We pack the path at the end of *dist_array and return
// a pointer to this part of the array. Example:
// dist_array = [1x2xx3x2] => packed [1x2x1232], chosen_path = [1232]
static void TraceBackwards(uint16_t* const dist_array,
                           int dist_array_size,
                           uint16_t** const chosen_path,
                           int* const chosen_path_size) {
  uint16_t* path = dist_array + dist_array_size;
  uint16_t* cur = dist_array + dist_array_size - 1;
  while (cur >= dist_array) {
    const int k = *cur;
    --path;
    *path = k;
    cur -= k;
  }
  *chosen_path = path;
  *chosen_path_size = (int)(dist_array + dist_array_size - path);
}

static int BackwardReferencesHashChainFollowChosenPath(
    const uint32_t* const argb, int cache_bits,
    const uint16_t* const chosen_path, int chosen_path_size,
    const VP8LHashChain* const hash_chain, VP8LBackwardRefs* const refs) {
  const int use_color_cache = (cache_bits > 0);
  int ix;
  int i = 0;
  int ok = 0;
  int cc_init = 0;
  VP8LColorCache hashers;

  if (use_color_cache) {
    cc_init = VP8LColorCacheInit(&hashers, cache_bits);
    if (!cc_init) goto Error;
  }

  ClearBackwardRefs(refs);
  for (ix = 0; ix < chosen_path_size; ++ix) {
    const int len = chosen_path[ix];
    if (len != 1) {
      int k;
      const int offset = HashChainFindOffset(hash_chain, i);
      BackwardRefsCursorAdd(refs, PixOrCopyCreateCopy(offset, len));
      if (use_color_cache) {
        for (k = 0; k < len; ++k) {
          VP8LColorCacheInsert(&hashers, argb[i + k]);
        }
      }
      i += len;
    } else {
      PixOrCopy v;
      const int idx =
          use_color_cache ? VP8LColorCacheContains(&hashers, argb[i]) : -1;
      if (idx >= 0) {
        // use_color_cache is true and hashers contains argb[i]
        // push pixel as a color cache index
        v = PixOrCopyCreateCacheIdx(idx);
      } else {
        if (use_color_cache) VP8LColorCacheInsert(&hashers, argb[i]);
        v = PixOrCopyCreateLiteral(argb[i]);
      }
      BackwardRefsCursorAdd(refs, v);
      ++i;
    }
  }
  ok = !refs->error_;
 Error:
  if (cc_init) VP8LColorCacheClear(&hashers);
  return ok;
}

// Returns 1 on success.
static int BackwardReferencesTraceBackwards(
    int xsize, int ysize, const uint32_t* const argb, int cache_bits,
    const VP8LHashChain* const hash_chain,
    const VP8LBackwardRefs* const refs_src, VP8LBackwardRefs* const refs_dst) {
  int ok = 0;
  const int dist_array_size = xsize * ysize;
  uint16_t* chosen_path = NULL;
  int chosen_path_size = 0;
  uint16_t* dist_array =
      (uint16_t*)WebPSafeMalloc(dist_array_size, sizeof(*dist_array));

  if (dist_array == NULL) goto Error;

  if (!BackwardReferencesHashChainDistanceOnly(
          xsize, ysize, argb, cache_bits, hash_chain, refs_src, dist_array)) {
    goto Error;
  }
  TraceBackwards(dist_array, dist_array_size, &chosen_path, &chosen_path_size);
  if (!BackwardReferencesHashChainFollowChosenPath(
          argb, cache_bits, chosen_path, chosen_path_size, hash_chain,
          refs_dst)) {
    goto Error;
  }
  ok = 1;
 Error:
  WebPSafeFree(dist_array);
  return ok;
}

static void BackwardReferences2DLocality(int xsize,
                                         const VP8LBackwardRefs* const refs) {
  VP8LRefsCursor c = VP8LRefsCursorInit(refs);
  while (VP8LRefsCursorOk(&c)) {
    if (PixOrCopyIsCopy(c.cur_pos)) {
      const int dist = c.cur_pos->argb_or_distance;
      const int transformed_dist = DistanceToPlaneCode(xsize, dist);
      c.cur_pos->argb_or_distance = transformed_dist;
    }
    VP8LRefsCursorNext(&c);
  }
}

// Evaluate optimal cache bits for the local color cache.
// The input *best_cache_bits sets the maximum cache bits to use (passing 0
// implies disabling the local color cache). The local color cache is also
// disabled for the lower (<= 25) quality.
// Returns 0 in case of memory error.
static int CalculateBestCacheSize(const uint32_t* argb, int quality,
                                  const VP8LBackwardRefs* const refs,
                                  int* const best_cache_bits) {
  int i;
  const int cache_bits_max = (quality <= 25) ? 0 : *best_cache_bits;
  double entropy_min = MAX_ENTROPY;
  int cc_init[MAX_COLOR_CACHE_BITS + 1] = { 0 };
  VP8LColorCache hashers[MAX_COLOR_CACHE_BITS + 1];
  VP8LRefsCursor c = VP8LRefsCursorInit(refs);
  VP8LHistogram* histos[MAX_COLOR_CACHE_BITS + 1] = { NULL };
  int ok = 0;

  assert(cache_bits_max >= 0 && cache_bits_max <= MAX_COLOR_CACHE_BITS);

  if (cache_bits_max == 0) {
    *best_cache_bits = 0;
    // Local color cache is disabled.
    return 1;
  }

  // Allocate data.
  for (i = 0; i <= cache_bits_max; ++i) {
    histos[i] = VP8LAllocateHistogram(i);
    if (histos[i] == NULL) goto Error;
    if (i == 0) continue;
    cc_init[i] = VP8LColorCacheInit(&hashers[i], i);
    if (!cc_init[i]) goto Error;
  }

  // Find the cache_bits giving the lowest entropy. The search is done in a
  // brute-force way as the function (entropy w.r.t cache_bits) can be
  // anything in practice.
  while (VP8LRefsCursorOk(&c)) {
    const PixOrCopy* const v = c.cur_pos;
    if (PixOrCopyIsLiteral(v)) {
      const uint32_t pix = *argb++;
      const uint32_t a = (pix >> 24) & 0xff;
      const uint32_t r = (pix >> 16) & 0xff;
      const uint32_t g = (pix >>  8) & 0xff;
      const uint32_t b = (pix >>  0) & 0xff;
      // The keys of the caches can be derived from the longest one.
      int key = VP8LHashPix(pix, 32 - cache_bits_max);
      // Do not use the color cache for cache_bits = 0.
      ++histos[0]->blue_[b];
      ++histos[0]->literal_[g];
      ++histos[0]->red_[r];
      ++histos[0]->alpha_[a];
      // Deal with cache_bits > 0.
      for (i = cache_bits_max; i >= 1; --i, key >>= 1) {
        if (VP8LColorCacheLookup(&hashers[i], key) == pix) {
          ++histos[i]->literal_[NUM_LITERAL_CODES + NUM_LENGTH_CODES + key];
        } else {
          VP8LColorCacheSet(&hashers[i], key, pix);
          ++histos[i]->blue_[b];
          ++histos[i]->literal_[g];
          ++histos[i]->red_[r];
          ++histos[i]->alpha_[a];
        }
      }
    } else {
      // We should compute the contribution of the (distance,length)
      // histograms but those are the same independently from the cache size.
      // As those constant contributions are in the end added to the other
      // histogram contributions, we can safely ignore them.
      int len = PixOrCopyLength(v);
      uint32_t argb_prev = *argb ^ 0xffffffffu;
      // Update the color caches.
      do {
        if (*argb != argb_prev) {
          // Efficiency: insert only if the color changes.
          int key = VP8LHashPix(*argb, 32 - cache_bits_max);
          for (i = cache_bits_max; i >= 1; --i, key >>= 1) {
            hashers[i].colors_[key] = *argb;
          }
          argb_prev = *argb;
        }
        argb++;
      } while (--len != 0);
    }
    VP8LRefsCursorNext(&c);
  }

  for (i = 0; i <= cache_bits_max; ++i) {
    const double entropy = VP8LHistogramEstimateBits(histos[i]);
    if (i == 0 || entropy < entropy_min) {
      entropy_min = entropy;
      *best_cache_bits = i;
    }
  }
  ok = 1;
Error:
  for (i = 0; i <= cache_bits_max; ++i) {
    if (cc_init[i]) VP8LColorCacheClear(&hashers[i]);
    VP8LFreeHistogram(histos[i]);
  }
  return ok;
}

// Update (in-place) backward references for specified cache_bits.
static int BackwardRefsWithLocalCache(const uint32_t* const argb,
                                      int cache_bits,
                                      VP8LBackwardRefs* const refs) {
  int pixel_index = 0;
  VP8LColorCache hashers;
  VP8LRefsCursor c = VP8LRefsCursorInit(refs);
  if (!VP8LColorCacheInit(&hashers, cache_bits)) return 0;

  while (VP8LRefsCursorOk(&c)) {
    PixOrCopy* const v = c.cur_pos;
    if (PixOrCopyIsLiteral(v)) {
      const uint32_t argb_literal = v->argb_or_distance;
      const int ix = VP8LColorCacheContains(&hashers, argb_literal);
      if (ix >= 0) {
        // hashers contains argb_literal
        *v = PixOrCopyCreateCacheIdx(ix);
      } else {
        VP8LColorCacheInsert(&hashers, argb_literal);
      }
      ++pixel_index;
    } else {
      // refs was created without local cache, so it can not have cache indexes.
      int k;
      assert(PixOrCopyIsCopy(v));
      for (k = 0; k < v->len; ++k) {
        VP8LColorCacheInsert(&hashers, argb[pixel_index++]);
      }
    }
    VP8LRefsCursorNext(&c);
  }
  VP8LColorCacheClear(&hashers);
  return 1;
}

static VP8LBackwardRefs* GetBackwardReferencesLowEffort(
    int width, int height, const uint32_t* const argb,
    int* const cache_bits, const VP8LHashChain* const hash_chain,
    VP8LBackwardRefs* const refs_lz77) {
  *cache_bits = 0;
  if (!BackwardReferencesLz77(width, height, argb, 0, hash_chain, refs_lz77)) {
    return NULL;
  }
  BackwardReferences2DLocality(width, refs_lz77);
  return refs_lz77;
}

static VP8LBackwardRefs* GetBackwardReferences(
    int width, int height, const uint32_t* const argb, int quality,
    int* const cache_bits, const VP8LHashChain* const hash_chain,
    VP8LBackwardRefs* const refs_lz77, VP8LBackwardRefs* const refs_rle) {
  int lz77_is_useful;
  int cache_bits_lz77 = *cache_bits, cache_bits_rle = *cache_bits;
  double bit_cost_lz77, bit_cost_rle;
  VP8LBackwardRefs* best = NULL;
  VP8LHistogram* histo = NULL;

  // Compute LZ77 with no cache (0 bits), as the ideal LZ77 with a color cache
  // is not that different in practice.
  if (!BackwardReferencesLz77(width, height, argb, 0, hash_chain, refs_lz77)) {
    goto Error;
  }
  if (!CalculateBestCacheSize(argb, quality, refs_lz77, &cache_bits_lz77)) {
    goto Error;
  }
  // Transform refs_lz77 for the optimized cache_bits_lz77.
  if (cache_bits_lz77 > 0) {
    if (!BackwardRefsWithLocalCache(argb, cache_bits_lz77, refs_lz77)) {
      goto Error;
    }
  }

  // RLE.
  if (!BackwardReferencesRle(width, height, argb, 0, refs_rle)) {
    goto Error;
  }
  if (!CalculateBestCacheSize(argb, quality, refs_rle, &cache_bits_rle)) {
    goto Error;
  }
  // Transform refs_rle for the optimized cache_bits_rle.
  if (cache_bits_rle > 0) {
    if (!BackwardRefsWithLocalCache(argb, cache_bits_rle, refs_rle)) {
      goto Error;
    }
  }

  {
    // Evaluate RLE coding.
    histo = VP8LAllocateHistogram(cache_bits_rle);
    if (histo == NULL) goto Error;
    VP8LHistogramCreate(histo, refs_rle, cache_bits_rle);
    bit_cost_rle = VP8LHistogramEstimateBits(histo);
    VP8LFreeHistogram(histo);
    // Evaluate LZ77 coding.
    histo = VP8LAllocateHistogram(cache_bits_lz77);
    if (histo == NULL) goto Error;
    VP8LHistogramCreate(histo, refs_lz77, cache_bits_lz77);
    bit_cost_lz77 = VP8LHistogramEstimateBits(histo);
    // Decide if LZ77 is useful.
    lz77_is_useful = (bit_cost_lz77 < bit_cost_rle);
  }

  // Choose appropriate backward reference.
  if (lz77_is_useful) {
    // TraceBackwards is costly. Don't execute it at lower quality.
    const int try_lz77_trace_backwards = (quality >= 25);
    best = refs_lz77;   // default guess: lz77 is better
    if (try_lz77_trace_backwards) {
      if (BackwardReferencesTraceBackwards(width, height, argb, cache_bits_lz77,
                                           hash_chain, refs_lz77, refs_rle)) {
        double bit_cost_trace;
        // Evaluate LZ77 coding.
        VP8LHistogramCreate(histo, refs_rle, cache_bits_lz77);
        bit_cost_trace = VP8LHistogramEstimateBits(histo);
        if (bit_cost_trace < bit_cost_lz77) {
          best = refs_rle;
        }
      }
    }
    *cache_bits = cache_bits_lz77;
  } else {
    best = refs_rle;
    *cache_bits = cache_bits_rle;
  }

  BackwardReferences2DLocality(width, best);

 Error:
  VP8LFreeHistogram(histo);
  return best;
}

VP8LBackwardRefs* VP8LGetBackwardReferences(
    int width, int height, const uint32_t* const argb, int quality,
    int low_effort, int* const cache_bits,
    const VP8LHashChain* const hash_chain, VP8LBackwardRefs* const refs_tmp1,
    VP8LBackwardRefs* const refs_tmp2) {
  if (low_effort) {
    return GetBackwardReferencesLowEffort(width, height, argb, cache_bits,
                                          hash_chain, refs_tmp1);
  } else {
    return GetBackwardReferences(width, height, argb, quality, cache_bits,
                                 hash_chain, refs_tmp1, refs_tmp2);
  }
}
