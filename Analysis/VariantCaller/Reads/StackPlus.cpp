/* Copyright (C) 2013 Ion Torrent Systems, Inc. All Rights Reserved */

//! @file     StackPlus.cpp
//! @ingroup  VariantCaller
//! @brief    HP Indel detection

#include "StackPlus.h"


// check if we're done with reading in alignments by some condition
bool StackPlus::CheckValidAlignmentPosition(const InputStructures &global_context, const BamTools::BamAlignment &alignment,string seqName,  int variant_start_pos) {

  string chr_i = "";
  chr_i = global_context.sequences[alignment.RefID];
  if (chr_i.length() == 0 || chr_i.compare("*") == 0 || chr_i.compare(seqName) != 0 )
    return(false); // stop once you have reached unmapped beads

  if (alignment.Position > variant_start_pos)
      return(false);

  return (true);
}

// ------------------------------------------------------------

// See if read spans variant or mapping qv is too low
bool StackPlus::AlignmentReadFilter(const InputStructures &global_context, const BamTools::BamAlignment &alignment, int variant_end_pos) {

  // Check whether this read should be skipped
  if (alignment.GetEndPosition() < variant_end_pos)
    return (false);

  if (alignment.MapQuality < global_context.min_map_qv) {
	num_map_qv_filtered++;
    return (false);
  }
  return (true);
}

// ------------------------------------------------------------

// Perform reservoir sampling on reads
void StackPlus::ReservoirSampleReads(ExtendedReadInfo &current_read, unsigned int max_coverage, int DEBUG) {

  if (read_stack.size() < max_coverage) {
    read_counter++;
    read_stack.push_back(current_read);
    if (DEBUG>2)
      cout << "Read no. " << read_stack.size() << ": " << current_read.alignment.Name
           << " Start: " << current_read.alignment.Position << " End: " << current_read.alignment.GetEndPosition()
           << " start SC: " << current_read.startSC << " end SC: " << current_read.endSC << " Forw.Strand: " << current_read.is_forward_strand
           << " Start Flow: " << current_read.start_flow << endl << current_read.alignment.QueryBases << endl;
  }
  else {
    read_counter++;
    // produces a uniformly distributed test_position between [0, read_counter-1]
    unsigned int test_position = ((double)RandGen.Rand() / ((double)RandGen.RandMax + 1.0)) * (double)read_counter;
    if (test_position < max_coverage)
      read_stack[test_position] = current_read;
  }
}

// ------------------------------------------------------------

// Read and process records appropriate for this variant; positions are zero based
void StackPlus::StackUpOneVariant(PersistingThreadObjects & thread_objects,
                                  int variant_start_pos, int variant_end_pos, vcf::Variant ** candidate_variant,
                                  ExtendParameters * parameters, InputStructures &global_context) {

  // Initialize random number generator for each stack -> ensure reproducibility
  RandGen.SetSeed(parameters->my_controls.RandSeed);

  baseDepth = atoi((*candidate_variant)->info["DP"].at(0).c_str());
  read_stack.reserve(min(baseDepth, parameters->my_controls.downSampleCoverage));
  read_stack.clear();  // reset the stack

  ExtendedReadInfo current_read(global_context.nFlows);
  string readGroup;
  bool validReadGroup = false;
  read_counter = 0;
  num_map_qv_filtered = 0;

  while (thread_objects.bamMultiReader.GetNextAlignment(current_read.alignment)) {

    // Check valid read group
    validReadGroup = false;
    if (!current_read.alignment.GetTag("RG", readGroup)) {
      cerr << "FATAL: Found BAM alignment with no Read Group ID at " << current_read.alignment.RefID << ":" << current_read.alignment.Position << endl;
      exit(-1);
    }
    //check if the read group belongs to the sample of interest
    for (size_t counter = 0; counter < parameters->ReadGroupIDVector.size(); counter++) {
      if (readGroup.compare(parameters->ReadGroupIDVector.at(counter)) == 0) {
        validReadGroup = true;
        break; //once Read group is found no need to check further
      }
    }

    // Check global conditions to stop reading in more alignments
    if (!CheckValidAlignmentPosition(global_context, current_read.alignment, (*candidate_variant)->sequenceName, variant_start_pos))
      break;
    // Go to next read if we're within range of the variant position but read group is not the desired one.
    if (!validReadGroup)
      continue;
    if (!AlignmentReadFilter(global_context, current_read.alignment, variant_end_pos))
      continue;
    if (current_read.UnpackThisRead(global_context, thread_objects.local_contig_sequence, global_context.DEBUG))
      ReservoirSampleReads(current_read, parameters->my_controls.downSampleCoverage, global_context.DEBUG);
  }
  if (read_stack.size()==0) {
    cerr << "Nonfatal: No reads found for " << (*candidate_variant)->sequenceName << "\t" << variant_start_pos << endl;
    no_coverage = true;
  }
  // make sure we know how to interpret this read
  flow_order = global_context.flowOrder;
  if (global_context.DEBUG>0)
    cout <<"Stacked up " << read_stack.size() << " reads for variant " << (*candidate_variant)->sequenceName << ":"
         <<  (*candidate_variant)->position << ". Base Depth: " << baseDepth << ". "
         << num_map_qv_filtered << " of " << read_counter << " reads had too low map qv." << endl;
}
