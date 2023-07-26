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



#include "htslibcpp.hpp"
#include "bam_outfile.hpp"
#include "bam_tpool.hpp"

using std::string;
using std::vector;
using std::cerr;
using std::endl;
using std::string;
using std::vector;





static size_t
merge_mates(const size_t range,
            bam_rec &one, bam_rec &two, bam_rec &merged) {

  if (!are_mates(one, two)) return -std::numeric_limits<int>::max();

  // arithmetic easier using base 0 so subtracting 1 from pos
  const int one_s = one.get_pos();
  const int one_e = one.get_endpos();
  const int two_s = two.get_pos();
  const int two_e = two.get_endpos();
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

//static size_t
//merge_mates(const size_t range,
            //bam1_t *one, bam1_t *two, bam1_t *merged) {

  //if (!are_mates(one, two)) return -std::numeric_limits<int>::max();

  //// arithmetic easier using base 0 so subtracting 1 from pos
  //const int one_s = one->core.pos;
  //const int one_e = bam_endpos(one);
  //const int two_s = two->core.pos;
  //const int two_e = bam_endpos(two);
  //assert(one_s >= 0 && two_s >= 0);

  //const int spacer = two_s - one_e;
  //if (spacer >= 0) {
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
    //merge_non_overlap(one, two, spacer, merged);
  //}
  //else {
    //const int head = two_s - one_s;
    //if (head >= 0) {
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
      //if (head > 0) {
        //merge_overlap(one, two, head, merged);
      //}
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
      //// *** ELSE ***
      //if (head == 0) { // keep the end with more ref bases
        //keep_better_end(one, two, merged);
      //}
    //}
    //else {
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
      //const int overlap = two_e - one_s;
      //if (overlap > 0) {
        //truncate_overlap(one, overlap, merged);
      //}
    //}
  //}

  //// if merging two ends caused strange things in the cigar, fix them.
  //correct_cigar(merged);

  //return two_e - one_s;
//}
//[>*******Above are functions for merging pair-end reads*******<]


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


static void
add_pg_line(const string &cmd, bam_header &hdr) {
  int err_code =
    sam_hdr_add_line(hdr.h, "PG", "ID", "DNMTOOLS", "VN",
                     VERSION, "CL", cmd.c_str(), NULL);
  if (err_code) throw dnmt_error(err_code, "failed to add pg header line");
}

static void
format(const string &cmd, const size_t n_threads,
       const string &inputfile, const string &outfile,
       const bool bam_format, const string &input_format,
       const size_t suff_len, const size_t max_frag_len) {

  int err_code = 0;

  //htsThreadPool the_thread_pool{hts_tpool_init(n_threads), 0};
  bam_tpool tpool(n_threads, 0);
  // open the hts files; assume already checked
  bam_infile hts(inputfile);
  if (!hts) throw dnmt_error("error opening input file");

  // set the threads
  err_code = tpool.set_io(hts);
  if (err_code < 0) throw dnmt_error("error setting threads");
  //err_code = hts_set_thread_pool(out, &the_thread_pool);
  //if (err_code < 0) throw dnmt_error("error setting threads");

  // headers: load the input file's header, and then update to the
  // output file's header, then write it and destroy; we will only use
  // the input file header.
  bam_header hdr_out(hts.get_hdr());
  if (!hdr_out) throw dnmt_error("failed create header");
  add_pg_line(cmd, hdr_out);

  bam_outfile out(outfile, hdr_out.h,  bam_format);
  err_code = tpool.set_io(out);
  if (err_code < 0) throw dnmt_error("error setting threads");

  // now process the reads
  bam_rec aln;
  bam_rec prev_aln;
  bam_rec merged;
  bool previous_was_merged = false;

  if (!get_bam(hts, prev_aln)) throw dnmt_error(err_code, "format:sam_read1");


  while (get_bam(hts, aln)) {
    standardize_format(input_format, aln);
    if (same_name(prev_aln, aln, suff_len)) {
      // below: essentially check for dovetail
      if (!aln.is_rev()) std::swap(prev_aln, aln);
      const size_t frag_len = merge_mates(max_frag_len, prev_aln, 
          aln, merged);
      if (frag_len > 0 && frag_len < max_frag_len) {
        if (merged.is_a_rich()) merged.flip();
        if (!(out << merged)) throw dnmt_error(err_code, "format:sam_write1");
      }
      else {
        if (prev_aln.is_a_rich()) prev_aln.flip();
        if (!(out << prev_aln)) throw dnmt_error(err_code, "format:sam_write1");
        if (aln.is_a_rich()) aln.flip();
        if (!(out << aln)) throw dnmt_error(err_code, "format:sam_write1");
      }
      previous_was_merged = true;
    }
    else {
      if (!previous_was_merged) {
        if (prev_aln.is_a_rich()) prev_aln.flip();
        if (!(out << prev_aln)) throw dnmt_error(err_code, "format:sam_write1");
      }
      previous_was_merged = false;
    }
    std::swap(prev_aln, aln);
  }
  if (err_code < -1) throw dnmt_error(err_code, "format:sam_read1");

  if (!previous_was_merged) {
    if (prev_aln.is_a_rich()) prev_aln.flip();
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




int 
main(int argc, const char **argv) {
  main_format(argc, argv);
}

