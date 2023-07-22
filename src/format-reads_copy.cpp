/* format: a command within dnmtools to ensure SAM and BAM format
 * reads are conforming to expectations of dnmtools software
 *
 * Copyright (C) 2020-2023 University of Southern California and
 *                         Andrew D. Smith
 *
 * Authors: Andrew Smith and Guilherme Sena
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

/* The output of this program should include mapped reads that are
 * T-rich, which might require reverse-complementing sequences, and
 * switching their strand, along with any tag that indicates T-rich
 * vs. A-rich.
 *
 * Focusing only on single end reads, for each supported read mapping
 * tool, we require a means of determining whether or not the read is
 * A-rich and then changing that format to indicate T-rich.
 */

#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <stdexcept>
#include <sstream>
#include <cstring>
#include <cstdint> // for [u]int[0-9]+_t
#include <algorithm>

#include <config.h>

// from HTSlib
#include <htslib/sam.h>
#include <htslib/thread_pool.h>

// from smithlab
#include "OptionParser.hpp"
#include "smithlab_utils.hpp"
#include "smithlab_os.hpp"

// from dnmtools
#include "dnmt_error.hpp"

#include "bam_record.hpp"

using std::string;
using std::vector;
using std::cerr;
using std::endl;
using std::string;
using std::vector;

const uint32_t cins = 1;  // copied from sam.h
const uint32_t csoft_clip = 4; // copied from sam.h
const uint32_t cigar_op_mask = 0xf; // copied from sam.h
const uint32_t cigar_type_mask = 0x3C1A7; // copied from sam.h


static inline uint32_t
cigar_op(const uint32_t c) {
  return c & cigar_op_mask; 
}

static inline uint32_t
cigar_type(const uint32_t c) {
  return cigar_type_mask >> ( cigar_op(c) << 1) & 3;
}

static inline bool
eats_ref(const uint32_t c) { 
  return cigar_type(c) & 2;
}

static inline bool
eats_query(const uint32_t c) { 
  return cigar_type(c) & 1;
}

static inline uint32_t
to_insertion(const uint32_t x) {
  return (x & ~cigar_op_mask) | cins;
}

static void
fix_internal_softclip(const size_t n_cigar, uint32_t *cigar) {
  if (n_cigar < 3) return;
  // find first non-softclip
  auto c_beg = cigar;
  auto c_end = cigar + n_cigar;

  while (!eats_ref(*c_beg) && ++c_beg != c_end);
  if (c_beg == c_end) throw dnmt_error("cigar eats no ref");

  while (!eats_ref(*(c_end-1)) && --c_end != c_beg);
  if (c_beg == c_end) throw dnmt_error("cigar eats no ref");

  for (auto c_itr = c_beg; c_itr != c_end; ++c_itr)
    if ((*c_itr & cigar_op_mask) == csoft_clip)
      *c_itr = to_insertion(*c_itr);
}

static inline uint32_t
to_softclip(const uint32_t x) {
  return (x & ~cigar_op_mask) | csoft_clip;
}

static void
fix_external_insertion(const size_t n_cigar, uint32_t *cigar) {
  if (n_cigar < 2) return;

  auto c_itr = cigar;
  const auto c_end = c_itr + n_cigar;

  for (; !eats_ref(*c_itr) && c_itr != c_end; ++c_itr)
    *c_itr = to_softclip(*c_itr);

  if (c_itr == c_end) throw dnmt_error("cigar eats no ref");

  c_itr = cigar + n_cigar - 1;
  for (; !eats_ref(*c_itr) && c_itr != cigar; --c_itr)
    *c_itr = to_softclip(*c_itr);
}

static size_t
merge_cigar_ops(const size_t n_cigar, uint32_t *cigar) {
  if (n_cigar < 2) return n_cigar;
  auto c_itr1 = cigar;
  auto c_end = c_itr1 + n_cigar;
  auto c_itr2 = c_itr1 + 1;
  auto op1 = bam_cigar_op(*c_itr1);
  while (c_itr2 != c_end) {
    auto op2 = bam_cigar_op(*c_itr2);
    if (op1 == op2) {
      *c_itr1 = bam_cigar_gen(bam_cigar_oplen(*c_itr1) +
                                  bam_cigar_oplen(*c_itr2), op1);
    }
    else {
      *(++c_itr1) = *c_itr2;
      op1 = op2;
    }
    ++c_itr2;
  }
  // another increment to move past final "active" element for c_itr1
  ++c_itr1;
  return std::distance(cigar, c_itr1);
}

static size_t
correct_cigar(bam_rec &b) {
  /* This function will change external insertions into soft clip
     operations. Not sure why those would be present. It will also
     change internal soft-clip operations into insertions. This could
     be needed if soft-clipped ends of reads were moved to the middle
     of a merged fragment. Finally, it will collapse adjacent
     identical operations. None of this impacts the seq/qual/aux which
     get moved as a block */

  uint32_t *cigar = b.get_cigar();
  size_t n_cigar = b.record->core.n_cigar;
  fix_external_insertion(n_cigar, cigar);
  fix_internal_softclip(n_cigar, cigar);

  // merge identical adjacent cigar ops and get new number of ops
  n_cigar = merge_cigar_ops(n_cigar, cigar);
  // difference in bytes to shift the internal data
  const size_t delta = (b.record->core.n_cigar - n_cigar) * sizeof(uint32_t);
  if (delta > 0) { // if there is a difference; do the shift
    const uint8_t *data_end = b.get_aux() + b.l_aux();
    uint8_t *seq = b.get_seq();
    const uint8_t *seq_copy = seq;
    std::copy(seq_copy, data_end, seq - delta);
    b.record->core.n_cigar = n_cigar; // and update number of cigar ops
  }
  return delta;
}


static inline void
complement_seq(char *first, char *last) {
  for (; first != last; ++first) {
    assert(valid_base(*first));
    *first = complement(*first);
  }
}

static inline void
reverse(char *a, char *b) {
  char *p1, *p2;
  for (p1 = a, p2 = b - 1; p2 > p1; ++p1, --p2) {
    *p1 ^= *p2;
    *p2 ^= *p1;
    *p1 ^= *p2;
    assert(valid_base(*p1) && valid_base(*p2));
  }
}


// return value is the number of cigar ops that are fully consumed in
// order to read n_ref, while "partial_oplen" is the number of bases
// that could be taken from the next operation, which might be merged
// with the other read.
static uint32_t
get_full_and_partial_ops(const uint32_t *cig_in, const uint32_t in_ops,
                         const uint32_t n_ref_full, uint32_t *partial_oplen) {
  // assume: n_ops <= size(cig_in) <= size(cig_out)
  size_t rlen = 0;
  uint32_t i = 0;
  for (i = 0; i < in_ops; ++i) {
    if (eats_ref(cig_in[i])) {
      if (rlen + bam_cigar_oplen(cig_in[i]) > n_ref_full)
        break;
      rlen += bam_cigar_oplen(cig_in[i]);
    }
  }
  *partial_oplen = n_ref_full - rlen;
  return i;
}


/* This table converts 2 bases packed in a byte to their reverse
 * complement. The input is therefore a unit8_t representing 2 bases.
 * It is assumed that the input uint8_t value is of form "xx" or "x-",
 * where 'x' a 4-bit number representing either A, C, G, T, or N and
 * '-' is 0000.  For example, the ouptut for "AG" is "CT". The format
 * "x-" is often used at the end of an odd-length sequence.  The
 * output of "A-" is "-T", and the output of "C-" is "-G", and so
 * forth. The user must handle this case separately.
 */
const uint8_t byte_revcom_table[] = {
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  8, 136, 72, 0, 40, 0, 0, 0, 24, 0, 0, 0, 0, 0, 0, 248,
  4, 132, 68, 0, 36, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 244,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  2, 130, 66, 0, 34, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 242,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  1, 129, 65, 0, 33, 0, 0, 0, 17, 0, 0, 0, 0, 0, 0, 241,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
  0,   0,  0, 0,  0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0,   0,
 15, 143, 79, 0, 47, 0, 0, 0, 31, 0, 0, 0, 0, 0, 0, 255
};


static inline void
revcom_byte_then_reverse(unsigned char *a, unsigned char *b) {
  unsigned char *p1, *p2;
  for (p1 = a, p2 = b - 1; p2 > p1; ++p1, --p2) {
    *p1 = byte_revcom_table[*p1];
    *p2 = byte_revcom_table[*p2];
    *p1 ^= *p2;
    *p2 ^= *p1;
    *p1 ^= *p2;
  }
  if (p1 == p2) *p1 = byte_revcom_table[*p1];
}

static void
revcomp_seq_by_byte(bam_rec &aln) {
  const size_t l_qseq = aln.qlen();
  auto seq = aln.get_seq();
  const size_t num_bytes = (l_qseq + 1) / 2;
  auto seq_end = seq + num_bytes;
  revcom_byte_then_reverse(seq, seq_end);
  if (l_qseq % 2 == 1) { // for odd-length sequences
    for (size_t i = 0; i < num_bytes - 1; i++) {
      // swap 4-bit chunks within consecutive bytes like this:
      // (----aaaa bbbbcccc dddd....) => (aaaabbbb ccccdddd ....)
      seq[i] = (seq[i] << 4) | (seq[i + 1] >> 4);
    }
    seq[num_bytes - 1] <<= 4;
  }
}



// places seq of b at the end of seq of c
// assumes 0 < c_seq_len - b_seq_len <= a_seq_len
// also assumes that c_seq_len has been figured out
// Also assumes the number of bytes allocated to sequence potion of c->data
// has been set to ceil((a_used_len + b_seq_len) / 2.0) where
// a_used_len = c_seq_len - b_seq_len
static void
merge_by_byte(const bam_rec &a, const bam_rec &b, bam_rec &c) {
  // ADS: (todo) need some functions for int_ceil and is_odd
  const size_t b_seq_len = b.qlen();
  const size_t c_seq_len = c.qlen();
  const size_t a_used_len = c_seq_len - b_seq_len;

  const bool is_a_odd = a_used_len % 2 == 1;
  const bool is_b_odd = b_seq_len % 2 == 1;
  const bool is_c_odd = c_seq_len % 2 == 1;

  const size_t a_num_bytes = (a_used_len + 1) / 2;
  const size_t b_num_bytes = (b_seq_len + 1) / 2;

  const size_t b_offset = is_a_odd && is_b_odd;

  const auto a_seq = a.get_seq();
  const auto b_seq = b.get_seq();
  auto c_seq = c.get_seq();

  std::copy_n(a_seq, a_num_bytes, c_seq);
  if (is_a_odd) {
    // c_seq looks like either [ aa aa aa aa ]
    //                      or [ aa aa aa a- ]
    c_seq[a_num_bytes - 1] &= 0xf0;
    c_seq[a_num_bytes - 1] |= is_b_odd ?
        byte_revcom_table[b_seq[b_num_bytes - 1]] :
        byte_revcom_table[b_seq[b_num_bytes - 1]] >> 4;
  }
  if (is_c_odd) {
    // c_seq looks like either [ aa aa aa aa ]
    //                      or [ aa aa aa ab ]
    for (size_t i = 0; i < b_num_bytes - 1; i++) {
      c_seq[a_num_bytes + i] =
          (byte_revcom_table[b_seq[b_num_bytes - i - 1]] << 4) |
          (byte_revcom_table[b_seq[b_num_bytes - i - 2]] >> 4);
    }
    c_seq[a_num_bytes + b_num_bytes - 1] = byte_revcom_table[b_seq[0]] << 4;
    // Here, c_seq is either [ aa aa aa aa bb bb bb b- ] (a even; b odd)
    //                    or [ aa aa aa ab bb bb bb b- ] (a odd; b odd)
  }
  else {
    for (size_t i = 0; i < b_num_bytes - b_offset; i++) {
      c_seq[a_num_bytes + i] =
          byte_revcom_table[b_seq[b_num_bytes - i - 1 - b_offset]];
    }
    // Here, c_seq is either [ aa aa aa aa bb bb bb bb ] (a even and b even)
    //                    or [ aa aa aa ab bb bb bb    ] (a odd and b odd)
  }
}



static inline bool
format_is_bam_or_sam(htsFile *hts) {
  const htsFormat *fmt = hts_get_format(hts);
  return fmt->category == sequence_data &&
         (fmt->format == bam || fmt->format == sam);
}

static void
flip_conversion(bam_rec &aln) {
  if (aln.record->core.flag & BAM_FREVERSE)
    aln.record->core.flag = aln.record->core.flag & (~BAM_FREVERSE);
  else
    aln.record->core.flag = aln.record->core.flag | BAM_FREVERSE;

  revcomp_seq_by_byte(aln);

  // ADS: don't like *(cv + 1) below, but no HTSlib function for it?
  uint8_t *cv = bam_aux_get(aln.record, "CV");
  if (!cv) throw dnmt_error("bam_aux_get failed for CV");
  *(cv + 1) = 'T';
}


static bool
are_mates(const bam_rec &one, const bam_rec &two) {
  return one.record->core.mtid == two.record->core.tid &&
         one.record->core.mpos == two.record->core.pos &&
         one.is_rev() != two.is_rev();
}

static int
truncate_overlap(const bam_rec &a, const uint32_t overlap, bam_rec &c) {

  const uint32_t *a_cig = a.get_cigar();
  const uint32_t a_ops = a.record->core.n_cigar;

  uint32_t part_op = 0;
  const uint32_t c_cur =
    get_full_and_partial_ops(a_cig, a_ops, overlap, &part_op);

  // ads: hack here because the get_full_and_partial_ops doesn't do
  // exactly what is needed for this.
  const bool use_partial = (c_cur < a.record->core.n_cigar && part_op > 0);

  const uint32_t c_ops = c_cur + use_partial;
  uint32_t *c_cig = (uint32_t *)calloc(c_ops, sizeof(uint32_t));

  // ads: replace this with a std::copy
  memcpy(c_cig, a_cig, c_cur * sizeof(uint32_t));
  // ads: warning, if !use_partial, the amount of part_op used below
  // would make no sense.
  if (use_partial)
    c_cig[c_cur] = bam_cigar_gen(part_op, bam_cigar_op(a_cig[c_cur]));
  /* after this point the cigar is set and should decide everything */

  const uint32_t c_seq_len = bam_cigar2qlen(c_ops, c_cig);
  const hts_pos_t isize = bam_cigar2rlen(c_ops, c_cig);

  // flag only needs to worry about strand and single-end stuff
  const uint16_t flag =
      a.record->core.flag & (BAM_FREAD1 | BAM_FREAD2 | BAM_FREVERSE);

  int ret = 
    bam_set1_wrapper(c,
                     a.record->core.l_qname - (a.record->core.l_extranul + 1),
                     bam_get_qname(a.record),
                     flag, // flags (SR and revcomp info)
                     a.record->core.tid,
                     a.record->core.pos,
                     a.record->core.qual,
                     c_ops,     // merged cigar ops
                     c_cig,     // merged cigar
                     -1,        // (no mate)
                     -1,        // (no mate)
                     isize,     // rlen from new cigar
                     c_seq_len, // truncated seq length
                     8);        // enough for the 2 tags?
  if (ret < 0) throw dnmt_error(ret, "bam_set1_wrapper");
  // ADS: might it be better to fill `c->data` directly?
  free(c_cig);

  auto c_seq = c.get_seq();
  size_t num_bytes_to_copy = (c_seq_len + 1)/2;
  std::copy_n(a.get_seq(), num_bytes_to_copy, c_seq);

  /* add the tags */
  const int64_t nm = bam_aux2i(bam_aux_get(a.record, "NM")); // ADS: do better here!
  // "udpate" for "int" because it determines the right size
  ret = c.aux_update_int("NM", nm);
  if (ret < 0) throw dnmt_error(ret, "bam_aux_update_int");

  const uint8_t conversion = bam_aux2A(bam_aux_get(a.record, "CV"));
  // "append" for "char" because there is no corresponding update
  ret = bam_aux_append(c.record, "CV", 'A', 1, &conversion);
  if (ret < 0) throw dnmt_error(ret, "bam_aux_append");

  return ret;
}

static int
merge_overlap(const bam_rec &a, const bam_rec &b,
              const uint32_t head, bam_rec &c) {
  assert(head > 0);

  const uint32_t *a_cig = a.get_cigar();
  const uint32_t a_ops = a.record->core.n_cigar;

  const uint32_t *b_cig = b.get_cigar();
  const uint32_t b_ops = b.record->core.n_cigar;

  uint32_t part_op = 0;
  uint32_t c_cur = get_full_and_partial_ops(a_cig, a_ops, head, &part_op);
  // ADS: hack here because the get_full_and_partial_ops doesn't do
  // exactly what is needed for this.
  const bool use_partial = (c_cur < a.record->core.n_cigar && part_op > 0);

  // check if the middle op would be the same
  const bool merge_mid =
      (use_partial > 0 ?
      bam_cigar_op(a_cig[c_cur]) == bam_cigar_op(b_cig[0]) :
      bam_cigar_op(a_cig[c_cur - 1]) == bam_cigar_op(b_cig[0]));

  // c_ops: include the prefix of a_cig we need; then add for the
  // partial op; subtract for the identical op in the middle; finally
  // add the rest of b_cig.
  const uint32_t c_ops = c_cur + use_partial - merge_mid + b_ops;

  uint32_t *c_cig = (uint32_t *)calloc(c_ops, sizeof(uint32_t));
  // std::fill(c_cig, c_cig + c_ops, std::numeric_limits<uint32_t>::max());
  memcpy(c_cig, a_cig, c_cur * sizeof(uint32_t));

  if (use_partial) {
    c_cig[c_cur] = bam_cigar_gen(part_op, bam_cigar_op(a_cig[c_cur]));
    c_cur++; // index of dest for copying b_cig; faciltates corner case
  }
  // Here we get the length of a's sequence part contribution to c's
  // sequence before the possibility of merging the last entry with
  // the first entry in b's cigar. This is done with the cigar, so
  // everything depends on the "use_partial"
  const size_t a_seq_len = bam_cigar2qlen(c_cur, c_cig);
  /* ADS: above the return type of bam_cigar2qlen is uint64_t, but
     according to the source as of 05/2023 it cannot become
     negative; no possible error code returned */

  if (merge_mid) // update the middle op if it's the same
    c_cig[c_cur - 1] = bam_cigar_gen(bam_cigar_oplen(c_cig[c_cur - 1]) +
                                     bam_cigar_oplen(b_cig[0]),
                                     bam_cigar_op(b_cig[0]));
  // copy the cigar from b into c
  memcpy(c_cig + c_cur, b_cig + merge_mid,
         (b_ops - merge_mid)*sizeof(uint32_t));
  /* done with cigar string here */

  const uint32_t c_seq_len = a_seq_len + b.record->core.l_qseq;

  // get the template length
  const hts_pos_t isize = bam_cigar2rlen(c_ops, c_cig);

  // flag only needs to worry about strand and single-end stuff
  uint16_t flag = (a.record->core.flag & (BAM_FREAD1 |
                                          BAM_FREAD2 |
                                          BAM_FREVERSE));

  int ret = bam_set1_wrapper(c,
                             a.record->core.l_qname - (a.record->core.l_extranul + 1),
                             bam_get_qname(a.record),
                             flag, // (no PE; revcomp info)
                             a.record->core.tid,
                             a.record->core.pos,
                             a.record->core.qual, // mapq from "a" (consider update)
                             c_ops,        // merged cigar ops
                             c_cig,        // merged cigar
                             -1,           // (no mate)
                             -1,           // (no mate)
                             isize,        // updated
                             c_seq_len,    // merged sequence length
                             8);           // enough for 2 tags?
  free(c_cig);
  if (ret < 0) throw dnmt_error(ret, "bam_set1_wrapper in merge_overlap");
  // Merge the sequences by bytes
  merge_by_byte(a, b, c);

  // add the tag for mismatches
  const int64_t nm = (bam_aux2i(bam_aux_get(a.record, "NM")) +
                      bam_aux2i(bam_aux_get(b.record, "NM")));
  ret = c.aux_update_int("NM", nm);
  if (ret < 0) throw dnmt_error(ret, "bam_aux_update_int in merge_overlap");

  // add the tag for conversion
  const uint8_t cv = bam_aux2A(bam_aux_get(a.record, "CV"));
  ret = bam_aux_append(c.record, "CV", 'A', 1, &cv);
  if (ret < 0) throw dnmt_error(ret, "bam_aux_append in merge_overlap");

  return ret;
}


static int
merge_non_overlap(const bam_rec &a, const bam_rec &b,
                  const uint32_t spacer, bam_rec &c) {
  /* make the cigar string */
  // collect info about the cigar strings
  const uint32_t *a_cig = a.get_cigar();
  const uint32_t a_ops = a.record->core.n_cigar;
  const uint32_t *b_cig = b.get_cigar();
  const uint32_t b_ops = b.record->core.n_cigar;
  // allocate the new cigar string
  const uint32_t c_ops = a_ops + b_ops + 1;
  uint32_t *c_cig = (uint32_t *)calloc(c_ops, sizeof(uint32_t));
  // concatenate the new cigar strings with a "skip" in the middle
  memcpy(c_cig, a_cig, a_ops * sizeof(uint32_t));
  c_cig[a_ops] = bam_cigar_gen(spacer, BAM_CREF_SKIP);
  memcpy(c_cig + a_ops + 1, b_cig, b_ops * sizeof(uint32_t));
  /* done with cigars */

  const size_t a_seq_len = a.record->core.l_qseq;
  const size_t b_seq_len = b.record->core.l_qseq;
  const size_t c_seq_len = a_seq_len + b_seq_len;

  // get the template length from the cigar
  const hts_pos_t isize = bam_cigar2rlen(c_ops, c_cig);

  // flag: only need to keep strand and single-end info
  const uint16_t flag = a.record->core.flag & (BAM_FREAD1 |
                                               BAM_FREAD2 |
                                               BAM_FREVERSE);

  int ret =
      bam_set1_wrapper(c,
                       a.record->core.l_qname - (a.record->core.l_extranul + 1),
                       bam_get_qname(a.record),
                       flag, // flags (no PE; revcomp info)
                       a.record->core.tid,
                       a.record->core.pos,
                       a.record->core.qual, // mapq from a (consider update)
                       c_ops,        // merged cigar ops
                       c_cig,        // merged cigar
                       -1,           // (no mate)
                       -1,           // (no mate)
                       isize,        // TLEN (relative to reference; SAM docs)
                       c_seq_len,    // merged sequence length
                       8);           // enough for 2 tags of 1 byte value?
  free(c_cig);
  if (ret < 0) throw dnmt_error(ret, "bam_set1 in merge_non_overlap");

  merge_by_byte(a, b, c);

  /* add the tags */
  const int64_t nm = (bam_aux2i(bam_aux_get(a.record, "NM")) +
                      bam_aux2i(bam_aux_get(b.record, "NM")));
  // "udpate" for "int" because it determines the right size
  ret = c.aux_update_int("NM", nm);
  if (ret < 0) throw dnmt_error(ret, "merge_non_overlap:bam_aux_update_int");

  const uint8_t cv = bam_aux2A(bam_aux_get(a.record, "CV"));
  // "append" for "char" because there is no corresponding update
  ret = bam_aux_append(c.record, "CV", 'A', 1, &cv);
  if (ret < 0) throw dnmt_error(ret, "merge_non_overlap:bam_aux_append");

  return ret;
}



static int
keep_better_end(const bam_rec &a, const bam_rec &b, bam_rec &c) {
  c.copy(a.rlen_from_cigar() >= b.rlen_from_cigar() ? a : b);
  c.record->core.mtid = -1;
  c.record->core.mpos = -1;
  c.record->core.isize = c.rlen_from_cigar();
  c.record->core.flag &= (BAM_FREAD1 | BAM_FREAD2 | BAM_FREVERSE);
  return 0;
}


static size_t
merge_mates(const size_t range,
            bam_rec &one, bam_rec &two, bam_rec &merged) {

  if (!are_mates(one, two)) return -std::numeric_limits<int>::max();

  // arithmetic easier using base 0 so subtracting 1 from pos
  const int one_s = one.pos();
  const int one_e = one.endpos();
  const int two_s = two.pos();
  const int two_e = two.endpos();
  assert(one_s >= 0 && two_s >= 0);

  const int spacer = two_s - one_e;
  if (spacer >= 0) {
    /* fragments longer enough that there is space between them: this
     * size of the spacer ("_") is determined based on the reference
     * positions of the two ends, and here we assume "one" maps to
     * positive genome strand.
     *                               spacer
     *                              <======>
     * left                                                         right
     * one_s                    one_e      two_s                    two_e
     * [------------end1------------]______[------------end2------------]
     */
    merge_non_overlap(one, two, spacer, merged);
  }
  else {
    const int head = two_s - one_s;
    if (head >= 0) {
      /* (Even if "head == 0" we will deal with it here.)
       *
       * CASE 1: head > 0
       *
       * fragment longer than or equal to the length of the left-most
       * read, but shorter than twice the read length (hence spacer
       * was < 0): this is determined by obtaining the size of the
       * "head" in the diagram below: the portion of end1 that is not
       * within [=]. If the read maps to the positive strand, this
       * depends on the reference start of end2 minus the reference
       * start of end1. For negative strand, this is reference start
       * of end1 minus reference start of end2.
       *
       * <======= head =========>
       *
       * left                                             right
       * one_s              two_s      one_e              two_e
       * [------------end1------[======]------end2------------]
       */
      if (head > 0) {
        merge_overlap(one, two, head, merged);
      }
      /* CASE 2: head == 0
       *
       * CASE 2A: one_e < two_e
       * left                                             right
       * one_s/two_s               one_e                  two_e
       * [=========== end1/end2========]------ end2 ----------]
       * keep "two"
       *
       * CASE 2B: one_e >= two_e
       * left                                             right
       * one_s/two_s               two_e                  one_e
       * [=========== end1/end2========]------ end1 ----------]
       * keep "one"
       *
       */
      // *** ELSE ***
      if (head == 0) { // keep the end with more ref bases
        keep_better_end(one, two, merged);
      }
    }
    else {
      /* dovetail fragments shorter than read length: this is
       * identified if the above conditions are not satisfied, but
       * there is still some overlap. The overlap will be at the 5'
       * ends of reads, which in theory shouldn't happen unless the
       * two ends are covering identical genomic intervals.
       *
       *                 <=== overlap ==>
       * left                                       right
       * two_s           one_s      two_e           one_e
       * [--end2---------[==============]---------end1--]
       */
      const int overlap = two_e - one_s;
      if (overlap > 0) {
        truncate_overlap(one, overlap, merged);
      }
    }
  }

  // if merging two ends caused strange things in the cigar, fix them.
  correct_cigar(merged);

  return two_e - one_s;
}
/********Above are functions for merging pair-end reads********/



// ADS: will move to using this function once it is written
// MN: below is the bam_rec version

static void
standardize_format(const string &input_format, bam_rec &aln) {
  int err_code = 0;

  if (input_format == "abismal" || input_format == "walt") return;

  if (input_format == "bsmap") {
    // A/T rich; get the ZS tag value
    auto zs_tag = bam_aux_get(aln.record, "ZS");
    if (!zs_tag) throw dnmt_error("bam_aux_get for ZS (invalid bsmap)");
    // ADS: test for errors on the line below
    const uint8_t cv = string(bam_aux2Z(zs_tag))[1] == '-' ? 'A' : 'T';
    // get the "mismatches" tag
    auto nm_tag = bam_aux_get(aln.record, "NM");
    if (!nm_tag) throw dnmt_error("bam_aux_get for NM (invalid bsmap)");
    const int64_t nm = bam_aux2i(nm_tag);

    aln.record->l_data = bam_get_aux(aln.record) - aln.record->data; // del aux (no data resize)

    /* add the tags we want */
    // "udpate" for "int" because it determines the right size; even
    // though we just deleted all tags, it will add it back here.
    err_code = bam_aux_update_int(aln.record, "NM", nm);
    if (err_code < 0) throw dnmt_error(err_code, "bam_aux_update_int");
    // "append" for "char" because there is no corresponding update
    err_code = bam_aux_append(aln.record, "CV", 'A', 1, &cv);
    if (err_code < 0) throw dnmt_error(err_code, "bam_aux_append");

    if (bam_is_rev(aln.record))
      revcomp_seq_by_byte(aln); // reverse complement if needed
  }
  if (input_format == "bismark") {
    // ADS: Previously we modified the read names at the first
    // underscore. Even if the names are still that way, it should no
    // longer be needed since we compare names up to a learned suffix.

    // A/T rich; get the XR tag value
    auto xr_tag = bam_aux_get(aln.record, "XR");
    if (!xr_tag) throw dnmt_error("bam_aux_get for XR (invalid bismark)");
    const uint8_t cv = string(bam_aux2Z(xr_tag)) == "GA" ? 'A' : 'T';
    // get the "mismatches" tag
    auto nm_tag = bam_aux_get(aln.record, "NM");
    if (!nm_tag) throw dnmt_error("bam_aux_get for NM (invalid bismark)");
    const int64_t nm = bam_aux2i(nm_tag);

    aln.record->l_data = bam_get_aux(aln.record) - aln.record->data; // del aux (no data resize)

    /* add the tags we want */
    // "udpate" for "int" because it determines the right size; even
    // though we just deleted all tags, it will add it back here.
    err_code = bam_aux_update_int(aln.record, "NM", nm);
    if (err_code < 0) throw dnmt_error(err_code, "bam_aux_update_int");
    // "append" for "char" because there is no corresponding update
    err_code = bam_aux_append(aln.record, "CV", 'A', 1, &cv);
    if (err_code < 0) throw dnmt_error(err_code, "bam_aux_append");

    if (bam_is_rev(aln.record))
      revcomp_seq_by_byte(aln); // reverse complement if needed
  }

  // Be sure this doesn't depend on mapper! Removes the "qual" part of
  // the data in a bam1_t struct but does not change its uncompressed
  // size.
  const auto qs = bam_get_qual(aln.record);
  std::fill(qs, qs + aln.record->core.l_qseq, '\xff'); // deletes qseq
}



static vector<string>
load_read_names(const string &inputfile, const size_t n_reads) {
  bam_infile hts(inputfile);
  if (!hts) throw dnmt_error("failed to open file: " + inputfile);

  bam_header hdr(hts.file->bam_header);
  if (!hdr.header) throw dnmt_error("failed to read header: " + inputfile);

  bam_rec aln;
  vector<string> names;
  size_t count = 0;
  while ((hts >> aln)  && count++ < n_reads)
    names.push_back(aln.qname());

  return names;
}


static size_t
get_max_repeat_count(const vector<string> &names, const size_t suff_len) {
  // assume "suff_len" is shorter than the shortest entry in "names"
  size_t repeat_count = 0;
  size_t tmp_repeat_count = 0;
  // allow the repeat_count to go to 2, which might not be the "max"
  // but still would indicate that this suffix length is too long and
  // would result in more that two reads identified mutually as mates.
  for (size_t i = 1; i < names.size() && repeat_count < 2; ++i) {
    if (names[i - 1].size() == names[i].size() &&
        equal(begin(names[i - 1]),
              end(names[i - 1]) - suff_len,
              begin(names[i])))
      ++tmp_repeat_count;
    else tmp_repeat_count = 0;
    repeat_count = std::max(repeat_count, tmp_repeat_count);
  }
  return repeat_count;
}

static bool
check_suff_len(const string &inputfile, const size_t suff_len,
               const size_t n_names_to_check) {
  /* thus function will indicate if the given suff_len would result in
     more than two reads being mutually considered mates */
  auto names(load_read_names(inputfile, n_names_to_check));
  // get the minimum read name length
  size_t min_name_len = std::numeric_limits<size_t>::max();
  for (auto &&i : names)
    min_name_len = std::min(min_name_len, i.size());
  if (min_name_len <= suff_len)
    throw dnmt_error("given suffix length exceeds min read name length");
  sort(begin(names), end(names));
  return get_max_repeat_count(names, suff_len) < 2;
}

static size_t
guess_suff_len(const string &inputfile, const size_t n_names_to_check,
               size_t &repeat_count) {

  // ADS: assuming copy elision but should test it
  auto names(load_read_names(inputfile, n_names_to_check));

  // get the minimum read name length
  size_t min_name_len = std::numeric_limits<size_t>::max();
  for (auto &&i : names)
    min_name_len = std::min(min_name_len, i.size());
  assert(min_name_len > 0);

  sort(begin(names), end(names));

  // these will be returned
  size_t suff_len = 0;
  repeat_count = 0;

  // check the possible read name suffix lengths; if any causes a
  // repeat count of more than 1 (here this means == 2), all greater
  // suffix lengths will also
  const size_t max_suff_len = min_name_len - 1;
  while (suff_len < max_suff_len && repeat_count == 0) {
    // check current suffix length guess
    repeat_count = get_max_repeat_count(names, suff_len);
    // we want to lag by one iteration
    if (repeat_count == 0)
      ++suff_len;
  }
  // repeat_count should be equal to the greatest value of
  // tmp_repeat_count over all inner iterations above. If this value
  // is not 1, it will indicate whether we have exactly hit a max
  // repeat count of 2, indicating mates, or exceeded it, indicating
  // there seems not to be a good suffix length to remove for
  // identifying mates

  return suff_len;
}

static string
remove_suff(const string &s, const size_t suff_len) {
  return s.size() > suff_len ? s.substr(0, s.size() - suff_len) : s;
}

static bool
check_sorted(const string &inputfile, const size_t suff_len, size_t n_reads) {
  // In order to check if mates are consecutive we need to check if a
  // given end has a mate and that mate is not adjacent. This requires
  // storing previous reads, not simply checking for adjacent pairs.
  auto names(load_read_names(inputfile, n_reads));
  for (auto &&i : names)
    i = remove_suff(i, suff_len);

  std::unordered_map<string, size_t> mate_lookup;
  for (size_t i = 0; i < names.size(); ++i) {
    auto the_mate = mate_lookup.find(names[i]);
    if (the_mate == end(mate_lookup)) // 1st time seeing this one
      mate_lookup[names[i]] = i;
    else if (the_mate->second != i - 1)
      return false;
  }
  // made it here: all reads with mates are consecutive
  return true;
}

static bool
check_input_file(const string &infile) {
  samFile* hts = hts_open(infile.c_str(), "r");
  if (!hts || errno) throw dnmt_error("error opening: " + infile);
  const htsFormat *fmt = hts_get_format(hts);
  if (fmt->category != sequence_data)
    throw dnmt_error("not sequence data: " + infile);
  if (fmt->format != bam && fmt->format != sam)
    throw dnmt_error("not SAM/BAM format: " + infile);

  const int err_code = hts_close(hts);
  if (err_code < 0) throw dnmt_error(err_code, "check_input_file:hts_close");

  return true;
}

static bool
check_format_in_header(const string &input_format, const string &inputfile) {
  samFile* hts = hts_open(inputfile.c_str(), "r");
  if (!hts) throw dnmt_error("error opening file: " + inputfile);

  sam_hdr_t *hdr = sam_hdr_read(hts);
  if (!hdr) throw dnmt_error("failed to read header: " + inputfile);

  auto begin_hdr = sam_hdr_str(hdr);
  auto end_hdr = begin_hdr + std::strlen(begin_hdr);
  auto it = std::search(begin_hdr, end_hdr,
                        begin(input_format), end(input_format),
                        [](const unsigned char a, const unsigned char b) {
                          return std::toupper(a) == std::toupper(b);
                        });
  bam_hdr_destroy(hdr);
  const int err_code = hts_close(hts);
  if (err_code < 0) throw dnmt_error(err_code, "check_format_in_header:hts_close");

  return it != end_hdr;
}


static bool
same_name(const bam_rec &a, const bam_rec &b, const size_t suff_len) {
  // "+ 1" below: extranul counts *extras*; we don't want *any* nulls
  const uint16_t a_l = a.record->core.l_qname - (a.record->core.l_extranul + 1);
  const uint16_t b_l = b.record->core.l_qname - (b.record->core.l_extranul + 1);
  if (a_l != b_l) return false;
  assert(a_l > suff_len);
  return !std::strncmp(bam_get_qname(a.record), 
      bam_get_qname(b.record), a_l - suff_len);
}

static void
add_pg_line(const string &cmd, sam_hdr_t *hdr) {
  int err_code =
    sam_hdr_add_line(hdr, "PG", "ID", "DNMTOOLS", "VN",
                     VERSION, "CL", cmd.c_str(), NULL);
  if (err_code) throw dnmt_error(err_code, "failed to add pg header line");
}

static void
format(const string &cmd, const size_t n_threads,
       const string &inputfile, const string &outfile,
       const bool bam_format, const string &input_format,
       const size_t suff_len, const size_t max_frag_len) {

  int err_code = 0;

  bam_tpool thread_pool(n_threads, 0);

  // open the hts files; assume already checked
  bam_infile hts(inputfile);

  // set the threads
  err_code = thread_pool.set_io(hts);
  if (err_code < 0) throw dnmt_error("error setting threads");

  // headers: load the input file's header, and then update to the
  // output file's header, then write it and destroy; we will only use
  // the input file header.
  bam_header hdr(hts.file->bam_header);
  if (!hdr.header) throw dnmt_error("failed to read header");
  bam_header hdr_out(hdr);
  if (!hdr_out.header) throw dnmt_error("failed create header");
  add_pg_line(cmd, hdr_out.header);

  bam_outfile out(outfile, hdr_out);

  err_code = thread_pool.set_io(out);
  if (err_code < 0) throw dnmt_error("error setting threads");

  // now process the reads
  bam_rec aln;
  bam_rec prev_aln;
  bam_rec merged;
  bool previous_was_merged = false;

  //err_code = sam_read1(hts.file, hdr.header, aln.record); // for EOF, err_code == -1
  if (!(hts >> prev_aln)) throw dnmt_error(err_code, "format:sam_read1");

  while (hts >> aln) {
    standardize_format(input_format, aln);
    if (same_name(prev_aln, aln, suff_len)) {
      // below: essentially check for dovetail
      if (!aln.is_rev()) swap(prev_aln, aln);
      const size_t frag_len = merge_mates(max_frag_len, prev_aln, 
          aln, merged);
      if (frag_len > 0 && frag_len < max_frag_len) {
        if (merged.is_a_rich()) flip_conversion(merged);
        if (!(out << merged)) throw dnmt_error(err_code, "format:sam_write1");
      }
      else {
        if (prev_aln.is_a_rich()) flip_conversion(prev_aln);
        if (!(out << prev_aln)) throw dnmt_error(err_code, "format:sam_write1");
        if (aln.is_a_rich()) flip_conversion(aln);
        if (!(out << aln)) throw dnmt_error(err_code, "format:sam_write1");
      }
      previous_was_merged = true;
    }
    else {
      if (!previous_was_merged) {
        if (prev_aln.is_a_rich()) flip_conversion(prev_aln);
        if (!(out << prev_aln)) throw dnmt_error(err_code, "format:sam_write1");
      }
      previous_was_merged = false;
    }
    swap(prev_aln, aln);
  }
  if (err_code < -1) throw dnmt_error(err_code, "format:sam_read1");

  if (!previous_was_merged) {
    if (prev_aln.is_a_rich()) flip_conversion(prev_aln);
    if (!(out << prev_aln)) throw dnmt_error(err_code, "format:sam_write1");
  }
}

int main_format(int argc, const char **argv) {

  try {

    size_t n_reads_to_check = 1000000;

    bool bam_format = false;
    bool use_stdout = false;

    string input_format;
    string outfile;
    int max_frag_len = std::numeric_limits<int>::max();
    size_t suff_len = 0;
    bool single_end = false;
    bool VERBOSE = false;
    bool force = false;
    size_t n_threads = 1;

    const string description = "convert SAM/BAM mapped bs-seq reads "
                               "to standard dnmtools format";

    /****************** COMMAND LINE OPTIONS ********************/
    OptionParser opt_parse(strip_path(argv[0]), description,
                           "<sam/bam-file> [out-file]", 2);
    opt_parse.add_opt("threads", 't', "number of threads", false, n_threads);
    opt_parse.add_opt("bam", 'B', "output in BAM format", false, bam_format);
    opt_parse.add_opt("stdout", '\0',
                      "write to standard output", false, use_stdout);
    opt_parse.add_opt("format", 'f', "input format {abismal, bsmap, bismark}",
                      false, input_format);
    opt_parse.add_opt("suff", 's', "read name suffix length",
                      false, suff_len);
    opt_parse.add_opt("single-end", '\0',
                      "assume single-end [do not use with -suff]",
                      false, single_end);
    opt_parse.add_opt("max-frag", 'L', "maximum allowed insert size",
                      false, max_frag_len);
    opt_parse.add_opt("check", 'c',
                      "check this many reads to validate read name suffix",
                      false, n_reads_to_check);
    opt_parse.add_opt("force", 'F', "force formatting for "
                      "mixed single and paired reads",
                      false, force);
    opt_parse.add_opt("verbose", 'v', "print more information",
                      false, VERBOSE);
    opt_parse.set_show_defaults();
    vector<string> leftover_args;
    opt_parse.parse(argc, argv, leftover_args);
    if (opt_parse.about_requested() || opt_parse.help_requested() ||
        leftover_args.empty()) {
      cerr << opt_parse.help_message() << endl
           << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.option_missing()) {
      cerr << opt_parse.option_missing_message() << endl;
      return EXIT_FAILURE;
    }
    if (suff_len != 0 && single_end) {
      cerr << "incompatible arguments specified" << endl
           << opt_parse.help_message() << endl;
      return EXIT_FAILURE;
    }
    if ((leftover_args.size() == 1 && !use_stdout) ||
        (leftover_args.size() == 2 && use_stdout)) {
      cerr << opt_parse.help_message() << endl
           << opt_parse.about_message() << endl;
      return EXIT_FAILURE;
    }
    const string infile(leftover_args.front());
    if (leftover_args.size() == 2 && !use_stdout)
      outfile = leftover_args.back();
    else
      outfile = string("-"); // so htslib can write to stdout
    /****************** END COMMAND LINE OPTIONS *****************/

    std::ostringstream cmd;
    copy(argv, argv + argc, std::ostream_iterator<const char *>(cmd, " "));

    if (VERBOSE)
      cerr << "[input file: " << infile << "]" << endl
           << "[mapper: " << input_format << "]" << endl
           << "[configuration: " << (single_end ? "SE" : "PE") << "]" << endl
           << "[output file: " << outfile << "]" << endl
           << "[output type: " << (bam_format ? "B" : "S") << "AM]" << endl
           << "[force formatting: " << (force ? "yes" : "no") << "]" << endl
           << "[threads requested: " << n_threads << "]" << endl
           << "[command line: \"" << cmd.str() << "\"]" << endl;

    check_input_file(infile);

    if (VERBOSE)
      if (!check_format_in_header(input_format, infile))
        cerr << "[warning: input format not found in header "
             << "(" << input_format << ", " << infile << ")]" << endl;

    if (!single_end && !force) {
      if (suff_len == 0) {
        size_t repeat_count = 0;
        suff_len = guess_suff_len(infile, n_reads_to_check, repeat_count);
        if (repeat_count > 1)
          throw dnmt_error("failed to identify read name suffix length\n"
                        "verify reads are not single-end\n"
                        "specify read name suffix length directly");
        if (VERBOSE)
          cerr << "[read name suffix length guess: " << suff_len << "]" << endl;
      }
      else if (!check_suff_len(infile, suff_len, n_reads_to_check))
        throw dnmt_error("wrong read name suffix length [" +
                      std::to_string(suff_len) + "] in: " + infile);
      if (!check_sorted(infile, suff_len, n_reads_to_check))
        throw dnmt_error("mates not consecutive in: " + infile);
    }

    if (VERBOSE && !single_end)
      cerr << "[readname suffix length: " << suff_len << "]" << endl;

    format(cmd.str(), n_threads, infile, outfile,
           bam_format, input_format, suff_len, max_frag_len);
  }
  catch (const std::exception &e) {
    cerr << e.what() << endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}


int main(int argc, const char **argv) {
  return main_format(argc, argv);
}


