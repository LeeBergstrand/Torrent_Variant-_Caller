/* Copyright (C) 2013 Ion Torrent Systems, Inc. All Rights Reserved */

#include <string>
#include <stdio.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <vector>
#include <map>
#include <deque>
#include <set>
#include <algorithm>
#include <stdlib.h>
#include <unistd.h>

#include "OptArgs.h"
#include "Utils.h"
#include "IonVersion.h"


using namespace std;


void PrepareHotspotsHelp()
{
  printf ("\n");
  printf ("tvcutils %s-%s (%s) - Miscellaneous tools used by Torrent Variant Caller plugin and workflow.\n",
      IonVersion::GetVersion().c_str(), IonVersion::GetRelease().c_str(), IonVersion::GetGitHash().c_str());
  printf ("\n");
  printf ("Usage:   tvcutils prepare_hotspots [options]\n");
  printf ("\n");
  printf ("General options:\n");
  printf ("  -b,--input-bed                 FILE       input hotspots in BED format [either -b or -v required]\n");
  printf ("  -v,--input-vcf                 FILE       input hotspots in VCF format [either -b or -v required]\n");
  printf ("  -d,--output-bed                FILE       output left-aligned hotspots in BED format [none]\n");
  printf ("  -o,--output-vcf                FILE       output post-processed hotspots in VCF format [none]\n");
  printf ("  -r,--reference                 FILE       reference fasta [required]\n");
  printf ("  -a,--left-alignment            on/off     perform left-alignment of indels [off]\n");
  printf ("  -s,--allow-block-substitutions on/off     do not filter out block substitution hotspots [on]\n");
  printf ("\n");
}


/**
 *
 * Requirements:
 *  - If input is BED, ignore ANCHOR, fetch correct anchor from fasta
 *  - If VCF, split multi-allelic entries
 *  - Verify reference bases match fasta. Show warning, ignore.
 *  - Verify OBS/ALT are valid bases. Show warning, ignore.
 *  - Verify OBS/ALT != REF. Show warning, ignore.
 *  - Migrate any remaining BED validator checks
 *  - Left align indels
 *  - Output VCF: Produce O* fields
 *  - Output VCF: Combine entries with common start
 *
 * Possibilities:
 *  - With VCF, propagate select INFO fields that may have useful annotations
 *  - Convert chromosome names: 1 -> chr1. Friendly to cosmic, dbsnp
 */

struct Allele;

struct LineStatus {
  LineStatus(int _line_number) : line_number(_line_number), filter_message_prefix(0), chr_idx(-1),opos(-1) {}
  int line_number;
  const char *filter_message_prefix;
  string filter_message;
//  Allele *allele;
  int chr_idx;
  long opos;
  string id;
};

struct Allele {
  int chr_idx;
  long pos, opos;
  string id;
  string ref, oref;
  string alt, oalt;
  map<string,string>  custom_tags;
  bool filtered;
  LineStatus *line_status;
};

bool compare_alleles (const Allele& a, const Allele& b)
{
  if (a.pos < b.pos)
    return true;
  if (a.pos > b.pos)
    return false;
  if (a.ref.length() < b.ref.length())
    return true;
  if (a.ref.length() > b.ref.length())
    return false;
  return a.alt < b.alt;
  //return a.pos < b.pos;
}


struct Reference {
  string chr;
  long size;
  const char *start;
  int bases_per_line;
  int bytes_per_line;

  char base(long pos) {
    if (pos < 0 or pos >= size)
      return 'N';
    long ref_line_idx = pos / bases_per_line;
    long ref_line_pos = pos % bases_per_line;
    return toupper(start[ref_line_idx*bytes_per_line + ref_line_pos]);
  }
};

int PrepareHotspots(int argc, const char *argv[])
{
  OptArgs opts;
  opts.ParseCmdLine(argc, argv);
  string input_bed_filename       = opts.GetFirstString ('b', "input-bed", "");
  string input_vcf_filename       = opts.GetFirstString ('v', "input-vcf", "");
  string output_bed_filename      = opts.GetFirstString ('d', "output-bed", "");
  string output_vcf_filename      = opts.GetFirstString ('o', "output-vcf", "");
  string reference_filename       = opts.GetFirstString ('r', "reference", "");
  bool left_alignment             = opts.GetFirstBoolean('a', "left-alignment", false);
  bool filter_bypass              = opts.GetFirstBoolean('f', "filter-bypass", false);
  bool allow_block_substitutions  = opts.GetFirstBoolean('s', "allow-block-substitutions", true);
  opts.CheckNoLeftovers();

  if((input_bed_filename.empty() == input_vcf_filename.empty()) or
      (output_bed_filename.empty() and output_vcf_filename.empty()) or reference_filename.empty()) {
    PrepareHotspotsHelp();
    return 1;
  }


  // Populate chromosome list from reference.fai
  // Use mmap to fetch the entire reference

  int ref_handle = open(reference_filename.c_str(),O_RDONLY);

  struct stat ref_stat;
  fstat(ref_handle, &ref_stat);
  char *ref = (char *)mmap(0, ref_stat.st_size, PROT_READ, MAP_SHARED, ref_handle, 0);


  FILE *fai = fopen((reference_filename+".fai").c_str(), "r");
  if (!fai) {
    fprintf(stderr, "ERROR: Cannot open %s.fai\n", reference_filename.c_str());
    return 1;
  }

  vector<Reference>  ref_index;
  map<string,int> ref_map;
  char line[1024], chrom_name[1024];
  while (fgets(line, 1024, fai) != NULL) {
    Reference ref_entry;
    long chr_start;
    if (5 != sscanf(line, "%1020s\t%ld\t%ld\t%d\t%d", chrom_name, &ref_entry.size, &chr_start,
                    &ref_entry.bases_per_line, &ref_entry.bytes_per_line))
      continue;
    ref_entry.chr = chrom_name;
    ref_entry.start = ref + chr_start;
    ref_index.push_back(ref_entry);
    ref_map[ref_entry.chr] = (int) ref_index.size() - 1;
  }
  fclose(fai);


  // Load input BED or load input VCF, group by chromosome

  deque<LineStatus> line_status;
  vector<deque<Allele> > alleles(ref_index.size());

  if (!input_bed_filename.empty()) {

    FILE *input = fopen(input_bed_filename.c_str(),"r");
    if (!input) {
      fprintf(stderr,"ERROR: Cannot open %s\n", input_bed_filename.c_str());
      return 1;
    }

    char line2[65536];

    int line_number = 0;
    bool line_overflow = false;
    while (fgets(line2, 65536, input) != NULL) {
      if (line2[0] and line2[strlen(line2)-1] != '\n' and strlen(line2) == 65535) {
        line_overflow = true;
        continue;
      }
      line_number++;
      if (line_overflow) {
        line_overflow = false;
        line_status.push_back(LineStatus(line_number));
        line_status.back().filter_message_prefix = "Malformed hotspot BED line: line length exceeds 64K";
        continue;
      }

      if (strncmp(line2, "browser", 7) == 0)
        continue;

      if (strncmp(line2, "track", 5) == 0) {
        if (string::npos != string(line2).find("allowBlockSubstitutions=true"))
          allow_block_substitutions = true;
        continue;
      }

      char *current_chr = strtok(line2, "\t\r\n");
      char *current_start = strtok(NULL, "\t\r\n");
      char *current_end = strtok(NULL, "\t\r\n");
      char *current_id = strtok(NULL, "\t\r\n");
      char *penultimate = strtok(NULL, "\t\r\n");
      char *ultimate = strtok(NULL, "\t\r\n");
      for (char *next = strtok(NULL, "\t\r\n"); next; next = strtok(NULL, "\t\r\n")) {
        penultimate = ultimate;
        ultimate = next;
      }

      if (!current_chr or !current_start or !current_end or !current_id or !penultimate or !ultimate) {
        line_status.push_back(LineStatus(line_number));
        line_status.back().filter_message_prefix = "Malformed hotspot BED line: expected at least 6 fields";
        continue;
      }

      Allele allele;

      string string_chr(current_chr);
      if (ref_map.find(string_chr) != ref_map.end())
        allele.chr_idx = ref_map[string_chr];
      else if (ref_map.find("chr"+string_chr) != ref_map.end())
        allele.chr_idx = ref_map["chr"+string_chr];
      else if (string_chr == "MT" and ref_map.find("chrM") != ref_map.end())
        allele.chr_idx = ref_map["chrM"];
      else {
        line_status.push_back(LineStatus(line_number));
        line_status.back().filter_message_prefix = "Unknown chromosome name: ";
        line_status.back().filter_message = string_chr;
        continue;
      }

      allele.pos = strtol(current_start,NULL,10);
      allele.id = current_id;

      char *current_ref = NULL;
      char *current_alt = NULL;
      for (char *next = strtok(penultimate, ";"); next; next = strtok(NULL, ";")) {
        if (strncmp(next,"REF=",4) == 0)
          current_ref = next;
        else if (strncmp(next,"OBS=",4) == 0)
          current_alt = next;
        else if (strncmp(next,"ANCHOR=",7) == 0) {
          // ignore ANCHOR
        } else {
          char *value = next;
          while (*value and *value != '=')
            ++value;
          if (*value == '=')
            *value++ = 0;
          allele.custom_tags[next] = value;
        }
      }
      if (!current_ref or !current_alt) {
        line_status.push_back(LineStatus(line_number));
        line_status.back().filter_message_prefix = "Malformed hotspot BED line: REF and OBS fields required in penultimate column";
        continue;
      }
      for (char *pos = current_ref+4; *pos; ++pos)
        allele.ref += toupper(*pos);
      for (char *pos = current_alt+4; *pos; ++pos)
        allele.alt += toupper(*pos);
      allele.filtered = false;
      line_status.push_back(LineStatus(line_number));
      allele.line_status = &line_status.back();
      allele.opos = allele.pos;
      allele.oref = allele.ref;
      allele.oalt = allele.alt;
      alleles[allele.chr_idx].push_back(allele);
      //line_status.back().allele = &alleles[allele.chr_idx].back();
      line_status.back().chr_idx = allele.chr_idx;
      line_status.back().opos = allele.opos;
      line_status.back().id = allele.id;
    }

    fclose(input);
  }


  if (!input_vcf_filename.empty()) {

    FILE *input = fopen(input_vcf_filename.c_str(),"r");
    if (!input) {
      fprintf(stderr,"ERROR: Cannot open %s\n", input_vcf_filename.c_str());
      return 1;
    }

    char line2[65536];
    int line_number = 0;
    bool line_overflow = false;
    while (fgets(line2, 65536, input) != NULL) {
      if (line2[0] and line2[strlen(line2)-1] != '\n' and strlen(line2) == 65535) {
        line_overflow = true;
        continue;
      }
      line_number++;
      if (line_overflow) {
        line_overflow = false;
        line_status.push_back(LineStatus(line_number));
        line_status.back().filter_message_prefix = "Malformed hotspot VCF line: line length exceeds 64K";
        continue;
      }

      if (strncmp(line2, "##allowBlockSubstitutions=true", 30) == 0) {
        allow_block_substitutions = true;
        continue;
      }
      if (line2[0] == '#')
        continue;

      char *current_chr = strtok(line2, "\t\r\n");
      char *current_start = strtok(NULL, "\t\r\n");
      char *current_id = strtok(NULL, "\t\r\n");
      char *current_ref = strtok(NULL, "\t\r\n");
      char *current_alt = strtok(NULL, "\t\r\n");
      strtok(NULL, "\t\r\n"); // Ignore QUAL
      strtok(NULL, "\t\r\n"); // Ignore FILTER
      char *current_info = strtok(NULL, "\t\r\n");

      if (!current_chr or !current_start or !current_id or !current_ref or !current_alt) {
        line_status.push_back(LineStatus(line_number));
        line_status.back().filter_message_prefix = "Malformed hotspot VCF line: expected at least 5 fields";
        continue;
      }


      string string_chr(current_chr);
      int chr_idx = 0;
      if (ref_map.find(string_chr) != ref_map.end())
        chr_idx = ref_map[string_chr];
      else if (ref_map.find("chr"+string_chr) != ref_map.end())
        chr_idx = ref_map["chr"+string_chr];
      else if (string_chr == "MT" and ref_map.find("chrM") != ref_map.end())
        chr_idx = ref_map["chrM"];
      else {
        line_status.push_back(LineStatus(line_number));
        line_status.back().filter_message_prefix = "Unknown chromosome name: ";
        line_status.back().filter_message = string_chr;
        continue;
      }

      for (char *pos = current_ref; *pos; ++pos)
        *pos = toupper(*pos);
      for (char *pos = current_alt; *pos; ++pos)
        *pos = toupper(*pos);


      // Process custom tags
      vector<string>  bstrand;
      vector<string>  hp_max_length;
      if (current_info) {
        string raw_oid;
        string raw_omapalt;
        string raw_bstrand;
        string raw_hp_max_length;
        for (char *next = strtok(current_info, ";"); next; next = strtok(NULL, ";")) {

          char *value = next;
          while (*value and *value != '=')
            ++value;
          if (*value == '=')
            *value++ = 0;

          if (strcmp(next, "TYPE") == 0)
            continue;
          if (strcmp(next, "HRUN") == 0)
            continue;
          if (strcmp(next, "HBASE") == 0)
            continue;
          if (strcmp(next, "FR") == 0)
            continue;
          if (strcmp(next, "OPOS") == 0)
            continue;
          if (strcmp(next, "OREF") == 0)
            continue;
          if (strcmp(next, "OALT") == 0)
            continue;
          if (strcmp(next, "OID") == 0) {
            raw_oid = value;
            continue;
          }
          if (strcmp(next, "OMAPALT") == 0) {
            raw_omapalt = value;
            continue;
          }
          if (strcmp(next, "BSTRAND") == 0) {
            raw_bstrand = value;
            continue;
          }
          if (strcmp(next, "hp_max_length") == 0) {
            raw_hp_max_length = value;
            continue;
          }
        }

        if (not raw_bstrand.empty())
          split(raw_bstrand, ',', bstrand);
        if (not raw_hp_max_length.empty())
          split(raw_hp_max_length, ',', hp_max_length);

      }


      unsigned int allele_idx = 0;
      for (char *sub_alt = strtok(current_alt,","); sub_alt; sub_alt = strtok(NULL,",")) {

        Allele allele;
        allele.chr_idx = chr_idx;
        allele.ref = current_ref;
        allele.alt = sub_alt;
        allele.pos = strtol(current_start,NULL,10)-1;
        allele.id = current_id;
        if (allele.id == ".")
          allele.id = "hotspot";

        allele.filtered = false;
        line_status.push_back(LineStatus(line_number));
        allele.line_status = &line_status.back();
        allele.opos = allele.pos;
        allele.oref = allele.ref;
        allele.oalt = allele.alt;

        if (allele_idx < bstrand.size()) {
          if (bstrand[allele_idx] != ".")
            allele.custom_tags["BSTRAND"] = bstrand[allele_idx];
        }

        if (allele_idx < hp_max_length.size()) {
          if (hp_max_length[allele_idx] != ".")
            allele.custom_tags["hp_max_length"] = hp_max_length[allele_idx];
        }

        alleles[allele.chr_idx].push_back(allele);
        //line_status.back().allele = &alleles[allele.chr_idx].back();
        line_status.back().chr_idx = allele.chr_idx;
        line_status.back().opos = allele.opos;
        line_status.back().id = allele.id;
        allele_idx++;
      }
    }

    fclose(input);
  }

  // Process by chromosome:
  //   - Verify reference allele
  //   - Left align
  //   - Sort
  //   - Filter for block substitutions, write

  FILE *output_vcf = NULL;
  if (!output_vcf_filename.empty()) {
    output_vcf = fopen(output_vcf_filename.c_str(), "w");
    if (!output_vcf) {
      fprintf(stderr,"ERROR: Cannot open %s for writing\n", output_vcf_filename.c_str());
      return 1;
    }
    fprintf(output_vcf, "##fileformat=VCFv4.1\n");
    if (allow_block_substitutions)
      fprintf(output_vcf, "##allowBlockSubstitutions=true\n");
    fprintf(output_vcf, "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\n");
  }
  FILE *output_bed = NULL;
  if (!output_bed_filename.empty()) {
    output_bed = fopen(output_bed_filename.c_str(), "w");
    if (!output_bed) {
      fprintf(stderr,"ERROR: Cannot open %s for writing\n", output_bed_filename.c_str());
      if (output_vcf)
        fclose(output_vcf);
      return 1;
    }
    if (allow_block_substitutions)
      fprintf(output_bed, "track name=\"hotspot\" type=bedDetail allowBlockSubstitutions=true\n");
    else
      fprintf(output_bed, "track name=\"hotspot\" type=bedDetail\n");
  }


  for (int chr_idx = 0; chr_idx < (int)ref_index.size(); ++chr_idx) {

    for (deque<Allele>::iterator A = alleles[chr_idx].begin(); A != alleles[chr_idx].end(); ++A) {

      // Invalid characters

      bool valid = true;
      for (const char *c = A->ref.c_str(); *c ; ++c)
        if (*c != 'A' and *c != 'C' and *c != 'G' and *c != 'T')
          valid = false;
      for (const char *c = A->alt.c_str(); *c ; ++c)
        if (*c != 'A' and *c != 'C' and *c != 'G' and *c != 'T')
          valid = false;
      if (not valid) {
        A->filtered = true;
        A->line_status->filter_message_prefix = "REF and/or ALT contain characters other than ACGT: ";
        A->line_status->filter_message = "REF = " + A->ref + " ALT = " + A->alt;
        continue;
      }

      // Filter REF == ALT

      if (A->ref == A->alt) {
        A->filtered = true;
        A->line_status->filter_message_prefix = "REF and ALT alleles equal";
        continue;
      }

      // Confirm reference allele.

      string ref_expected;
      for (int idx = 0; idx < (int) A->ref.size(); ++idx)
        ref_expected += ref_index[chr_idx].base(A->pos + idx);
      if (A->ref != ref_expected) {
        A->filtered = true;
        A->line_status->filter_message_prefix = "Provided REF allele does not match reference: ";
        A->line_status->filter_message = "Expected " + ref_expected + ", found " + A->ref;
        continue;
      }

      // Trim

      int ref_start = 0;
      int ref_end = A->ref.size();
      int alt_end = A->alt.size();

      // Option 1: trim all trailing bases

      //while(ref_end and alt_end and A->ref[ref_end-1] == A->alt[alt_end-1]) {
      //  --ref_end;
      //  --alt_end;
      //}

      // Option 2: trim all leading basees

      //while (ref_start < ref_end and ref_start < alt_end and A->ref[ref_start] == A->alt[ref_start])
      //  ++ref_start;


      // Option 3: trim anchor base if vcf

      if (!input_vcf_filename.empty()) {
        if (ref_end and alt_end and (ref_end == 1 or alt_end == 1) and A->ref[0] == A->alt[0])
          ref_start = 1;
      }

      A->pos += ref_start;
      A->ref = A->ref.substr(ref_start, ref_end-ref_start);
      A->alt = A->alt.substr(ref_start, alt_end-ref_start);
      ref_end -= ref_start;
      alt_end -= ref_start;

      // Left align
      if (left_alignment) {
        while (A->pos > 0) {
          char nuc = ref_index[chr_idx].base(A->pos-1);
          if (ref_end > 0 and A->ref[ref_end-1] != nuc)
            break;
          if (alt_end > 0 and A->alt[alt_end-1] != nuc)
            break;
          A->ref = string(1,nuc) + A->ref;
          A->alt = string(1,nuc) + A->alt;
          A->pos--;
        }
      }
      A->ref.resize(ref_end);
      A->alt.resize(alt_end);


      // Filter block substitutions: take 1

      if (ref_end > 0 and alt_end > 0 and ref_end != alt_end and not allow_block_substitutions and not filter_bypass) {
        A->filtered = true;
        A->line_status->filter_message_prefix = "Block substitutions not supported";
        continue;
      }

    }



    if (output_bed) {
      // Sort - without anchor base
      stable_sort(alleles[chr_idx].begin(), alleles[chr_idx].end(), compare_alleles);

      // Write
      for (deque<Allele>::iterator I = alleles[chr_idx].begin(); I != alleles[chr_idx].end(); ++I) {
        if (I->filtered)
          continue;

        fprintf(output_bed, "%s\t%ld\t%ld\t%s\tREF=%s;OBS=%s",
            ref_index[chr_idx].chr.c_str(), I->pos, I->pos + I->ref.size(), I->id.c_str(),
            I->ref.c_str(), I->alt.c_str());

        for (map<string,string>::iterator C = I->custom_tags.begin(); C != I->custom_tags.end(); ++C)
          fprintf(output_bed, ";%s=%s", C->first.c_str(), C->second.c_str());

        fprintf(output_bed, "\tNONE\n");

        /*
        if (I->pos)
          fprintf(output_bed, "%s\t%ld\t%ld\t%s\t0\t+\tREF=%s;OBS=%s;ANCHOR=%c\tNONE\n",
              ref_index[chr_idx].chr.c_str(), I->pos, I->pos + I->ref.size(), I->id.c_str(),
              I->ref.c_str(), I->alt.c_str(), ref_index[chr_idx].base(I->pos-1));
        else
          fprintf(output_bed, "%s\t%ld\t%ld\t%s\t0\t+\tREF=%s;OBS=%s;ANCHOR=\tNONE\n",
              ref_index[chr_idx].chr.c_str(), I->pos, I->pos + I->ref.size(), I->id.c_str(),
              I->ref.c_str(), I->alt.c_str());
        */
      }
    }


    if (output_vcf) {

      // Add anchor base to indels
      for (deque<Allele>::iterator I = alleles[chr_idx].begin(); I != alleles[chr_idx].end(); ++I) {
        if (I->filtered)
          continue;
        if (not I->ref.empty() and not I->alt.empty())
          continue;
        if (I->pos == 0) {
          I->filtered = true;
          I->line_status->filter_message_prefix = "INDELs at chromosome start not supported";
          continue;
        }
        I->pos--;
        I->ref = string(1,ref_index[chr_idx].base(I->pos)) + I->ref;
        I->alt = string(1,ref_index[chr_idx].base(I->pos)) + I->alt;
      }

      // Sort - with anchor base
      stable_sort(alleles[chr_idx].begin(), alleles[chr_idx].end(), compare_alleles);


      // Merge alleles, remove block substitutions, write
      for (deque<Allele>::iterator A = alleles[chr_idx].begin(); A != alleles[chr_idx].end(); ) {

        string max_ref;
        deque<Allele>::iterator B = A;
        for (; B != alleles[chr_idx].end() and B->pos == A->pos; ++B)
          if (!B->filtered and max_ref.size() < B->ref.size())
            max_ref = B->ref;

        bool filtered = true;
        map<string,set<string> > unique_alts_and_ids;
        for (deque<Allele>::iterator I = A; I != B; ++I) {
          if (I->filtered)
            continue;

          string new_alt = I->alt + max_ref.substr(I->ref.size());

          if (new_alt.size() > 1 and max_ref.size() > 1 and new_alt.size() != max_ref.size() and not allow_block_substitutions and not filter_bypass) {
            I->filtered = true;
            I->line_status->filter_message_prefix = "Block substitutions not supported (post-merge)";
            continue;
          }

          I->ref = max_ref;
          I->alt = new_alt;

          // Filter alleles with duplicate ALT + ID pairs
          map<string,set<string> >::iterator alt_iter = unique_alts_and_ids.find(new_alt);
          if (alt_iter != unique_alts_and_ids.end()) {
            if (alt_iter->second.count(I->id) > 0) {
              I->filtered = true;
              I->line_status->filter_message_prefix = "Duplicate allele and ID";
              continue;
            }
          }
          unique_alts_and_ids[new_alt].insert(I->id);

          filtered = false;
        }

        if (not filtered) {



          fprintf(output_vcf, "%s\t%ld\t.\t%s\t",
              ref_index[chr_idx].chr.c_str(), A->pos+1, max_ref.c_str());

          bool comma = false;

          map<string,map<string,string> > unique_alts_and_tags;
          set<string> unique_tags;

          for (deque<Allele>::iterator I = A; I != B; ++I) {
            if (I->filtered)
              continue;
            unique_alts_and_tags[I->alt].insert(I->custom_tags.begin(), I->custom_tags.end());
            for (map<string,string>::iterator S = I->custom_tags.begin(); S != I->custom_tags.end(); ++S)
              unique_tags.insert(S->first);
            /*
            if (unique_alt_alleles.count(I->alt) > 0)
              continue;
            unique_alt_alleles.insert(I->alt);
            if (comma)
              fprintf(output_vcf, ",");
            comma = true;
            fprintf(output_vcf, "%s", I->alt.c_str());
            */
          }

          for (map<string,map<string,string> >::iterator Q = unique_alts_and_tags.begin(); Q != unique_alts_and_tags.end(); ++Q) {
            if (comma)
              fprintf(output_vcf, ",");
            comma = true;
            fprintf(output_vcf, "%s", Q->first.c_str());
          }

          fprintf(output_vcf, "\t.\t.\tOID=");
          comma = false;
          for (deque<Allele>::iterator I = A; I != B; ++I) {
            if (I->filtered)
              continue;
            if (comma)
              fprintf(output_vcf, ",");
            comma = true;
            fprintf(output_vcf, "%s", I->id.c_str());
          }

          fprintf(output_vcf, ";OPOS=");
          comma = false;
          for (deque<Allele>::iterator I = A; I != B; ++I) {
            if (I->filtered)
              continue;
            if (comma)
              fprintf(output_vcf, ",");
            comma = true;
            fprintf(output_vcf, "%ld", I->opos+1);
          }

          fprintf(output_vcf, ";OREF=");
          comma = false;
          for (deque<Allele>::iterator I = A; I != B; ++I) {
            if (I->filtered)
              continue;
            if (comma)
              fprintf(output_vcf, ",");
            comma = true;
            fprintf(output_vcf, "%s", I->oref.c_str());
          }

          fprintf(output_vcf, ";OALT=");
          comma = false;
          for (deque<Allele>::iterator I = A; I != B; ++I) {
            if (I->filtered)
              continue;
            if (comma)
              fprintf(output_vcf, ",");
            comma = true;
            fprintf(output_vcf, "%s", I->oalt.c_str());
          }

          fprintf(output_vcf, ";OMAPALT=");
          comma = false;
          for (deque<Allele>::iterator I = A; I != B; ++I) {
            if (I->filtered)
              continue;
            if (comma)
              fprintf(output_vcf, ",");
            comma = true;
            fprintf(output_vcf, "%s", I->alt.c_str());
          }

          for (set<string>::iterator S = unique_tags.begin(); S != unique_tags.end(); ++S) {
            fprintf(output_vcf, ";%s=", S->c_str());
            comma=false;
            for (map<string,map<string,string> >::iterator Q = unique_alts_and_tags.begin(); Q != unique_alts_and_tags.end(); ++Q) {
              if (comma)
                fprintf(output_vcf, ",");
              comma = true;
              map<string,string>::iterator W = Q->second.find(*S);
              if (W == Q->second.end())
                fprintf(output_vcf, ".");
              else
                fprintf(output_vcf, "%s", W->second.c_str());
            }
          }
//            fprintf(output_vcf, ";%s=%s", S->first.c_str(), S->second.c_str());

          fprintf(output_vcf, "\n");
        }

        A = B;
      }
    }
  }



  if (output_bed) {
    fflush(output_bed);
    fclose(output_bed);
  }
  if (output_vcf) {
    fflush(output_vcf);
    fclose(output_vcf);
  }


  int lines_ignored = 0;
  for (deque<LineStatus>::iterator L = line_status.begin(); L != line_status.end(); ++L) {
    if (L->filter_message_prefix) {
      if (L->chr_idx >= 0)
        printf("Line %d ignored: [%s:%ld %s] %s%s\n", L->line_number, ref_index[L->chr_idx].chr.c_str(), L->opos+1, L->id.c_str(),
            L->filter_message_prefix, L->filter_message.c_str());
      else
        printf("Line %d ignored: %s%s\n", L->line_number, L->filter_message_prefix, L->filter_message.c_str());
      lines_ignored++;
    }
  }
  printf("Ignored %d out of %d lines\n", lines_ignored, (int)line_status.size());


  munmap(ref, ref_stat.st_size);
  close(ref_handle);

  return 0;
}



