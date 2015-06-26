/* Copyright (C) 2011 Ion Torrent Systems, Inc. All Rights Reserved */

//! @file     DPTreephaser.h
//! @ingroup  BaseCaller
//! @brief    DPTreephaser. Perform dephasing and call base sequence by tree search

#ifndef DPTREEPHASER_H
#define DPTREEPHASER_H

#include <vector>
#include <stddef.h>
#include <algorithm>
#include "BaseCallerUtils.h"
#include "SystemMagicDefines.h"
#include "PIDloop.h"

using namespace std;


//! @brief    Input/output data structure for DPTreephaser
//! @ingroup  BaseCaller

struct BasecallerRead {

  void SetData(const vector<float> &measurements, int num_flows);
  void SetDataAndKeyNormalize(const float *measurements, int num_flows, const int *key_flows, int num_key_flows);
  void SetDataAndKeyNormalizeNew(const float *measurements, int num_flows, const int *key_flows, int num_key_flows, const bool phased = false);

  float           key_normalizer;           //!< Scaling factor used for initial key normalization
  vector<float>   raw_measurements;         //!< Measured, key-normalized flow signal
  vector<float>   normalized_measurements;  //!< Measured flow signal with best normalization so far
  vector<float>   prediction;               //!< Model-based phased signal predicted for the "solved" sequence
  vector<char>    sequence;                 //!< Vector of ACGT bases. Output from Solver, input to Simulator

  // For QV metrics
  vector<float>   additive_correction;      //!< Additive correction applied to get normalized measurements
  vector<float>   multiplicative_correction;//!< Multiplicative correction applied to get normalized measurements
  vector<float>   state_inphase;            //!< Fraction of live in-phase polymerase
  vector<float>   state_total;              //!< Fraction of live polymerase
  vector<float>   penalty_residual;         //!< Absolute score of the called nuc hypothesis
  vector<float>   penalty_mismatch;         //!< Score difference to second-best nuc hypothesis

  // Nuc gain data
  const static float  kZeromerMin   = -0.20f;       //!< Key flow corrected non-key flow zeromer 3-sigma minimum
  const static float  kZeromerMax   =  0.37f;       //!< Key flow corrected non-key flow zeromer 3-sigma maximum
  const static float  kOnemerMin    =  0.50f;       //!< Key flow corrected non-key flow onemer 3-sigma minimum
  const static float  kOnemerMax    =  1.35f;       //!< Key flow corrected non-key flow onemer 3-sigma maximum
  const static float  kZeromerMean  = 0.08555f;     //!< Non-key flow zeromer mean, based on 10 million nucs;
  const static float  kOnemerMean   = 0.90255f;     //!< Non-key flow zeromer mean, based on 10 million nucs;
  const static float  kRunZeroSigSq = 0.0078146f;   //!< Non-key flow zeromer sigma squared
  const static float  kRunOneSigSq  = 0.015178f;    //!< Non-key flow onemer sigma squared
  const static float  kInvZeroSigSq = 127.9849f;    //!< Non-key flow zeromer sigma squared inverse (1/sig^2)
  const static float  kInvOneSigSq  = 65.88379f;    //!< Non-key flow onemer sigma squared inverse (1/sig^2)
};


const  int    kMinWindowSize_     = 20;   //!< Minimum normalization window size
const  int    kMaxWindowSize_     = 60;   //!< Maximum normalization window size


//! @brief    Performs dephasing and base calling by tree search
//! @ingroup  BaseCaller
//! @details
//! DPTreephaser is responsible for determining base sequence from phased signal output by background model.
//! It uses a generative phasing model that can produce expected signal (prediction) for a partial
//! or complete base sequence. It further uses tree search to find a base sequence with prediction
//! matching the background model signal (measurements) the closest.
//! Additionally, DPTreephaser contains signal normalization procedures that can correct for additive
//! and multiplicative distortion using earlier predicted signal.
//! This allows dephasing and normalization to be performed iteratively (adaptive normalization)

class DPTreephaser {

public:
    // These need to be public for TreephaserSSE to use.
    const static int    kWindowSizeDefault_ = 38;   //!< Default normalization window size

  //! @brief  Constructor.
  //! @param[in] flow_order   Flow order object, also stores number of flows
  DPTreephaser(const ion::FlowOrder& flow_order, const int windowSize=kWindowSizeDefault_);

  //! @brief  Set the normalization window size
  //! @param[in]  windowSize  Size of the normalization window to use.
  inline void SetNormalizationWindowSize(const int windowSize) { windowSize_ = max(kMinWindowSize_, min(windowSize, kMaxWindowSize_));}

  //! @brief  Initializes phasing model using specific phasing parameters.
  //! @param[in]  cf          Carry forward rate, how much nuc from previous flow is encountered
  //! @param[in]  ie          Incomplete extension rate, how much polymerase fails to incorporate
  //! @param[in]  dr          Droop, how much polymerase deactivates during an incorporation
  void  SetModelParameters(double cf, double ie, double dr);

  //! @brief  Initializes phasing model using specific phasing parameters.
  //! @param[in]  cf          Carry forward rate, how much nuc from previous flow is encountered
  //! @param[in]  ie          Incomplete extension rate, how much polymerase fails to incorporate
  void  SetModelParameters(double cf, double ie);

  //! @brief  Perform adaptive normalization using WindowedNormalize (slow on long reads)
  //! @param[in,out]  read      Input and output information for the read
  //! @param[in]      max_flows Number of flows to process
  void  NormalizeAndSolve_Adaptive(BasecallerRead& read, int max_flows);

  //! @brief  Perform adaptive normalization using Normalize (not as accurate)
  //! @param[in,out]  read      Input and output information for the read
  //! @param[in]      max_flows Number of flows to process
  void  NormalizeAndSolve_GainNorm(BasecallerRead& read, int max_flows);

  //! @brief  Perform adaptive normalization using WindowedNormalize and solving with sliding window
  //! @param[in,out]  read      Input and output information for the read
  //! @param[in]      max_flows Number of flows to process
  void  NormalizeAndSolve_SWnorm(BasecallerRead& read, int max_flows);

  //! @brief  Tree-search-based dephasing.
  //! @param[in]  read.normalized_measurements    Normalized measurements
  //! @param[out] read.sequence   Predicted base sequence
  //! @param[out] read.prediction Predicted signal
  //! @param[in]  max_flows       Number of flows to process
  //! @param[in]  restart_flows   Number of flows to simulate, rather than solve
  void  Solve(BasecallerRead& read, int max_flows, int restart_flows = 0);

  //! @brief  Generate predicted signal from base sequence
  //! @param[in]  read.sequence     Base sequence
  //! @param[out] read.prediction   Predicted signal
  //! @param[in]  max_flows         Number of flows to process
  void  Simulate(BasecallerRead& read, int max_flows, bool state_inphase=false);

  //! @brief  Applies signal recalibration to previously computed predicted sequence
  //! @param[in/out] read.predictions
  void RecalibratePredictions(BasecallerRead& data);

  //! @brief  Computes the state vector at a query main incorporating flow
  //! @param[in]  read.sequence     Base sequence
  //! @param[out] query_state       State vector
  //! @param[out] current_hp        Homopolymer length incorporating at query_flow
  //! @param[in]  max_flows         Vector size of query_state
  //! @param[in]  query_flow        Flow at which to compute state vector
  void QueryState(BasecallerRead& data, vector<float>& query_state, int& current_hp, int max_flows, int query_flow);

  //! @brief  Simulates a sequence and returns the progression of state vectors
  //! @param[in]  read.sequence     Base sequence
  //! @param[out] query_states      State vectors
  //! @param[out] hp_lengths        Vector of homopolymer lengths in the sequence
  //! @param[in]  max_flows         Size of a state vector
  void QueryAllStates(BasecallerRead& data, vector< vector<float> >& query_states, vector<int>& hp_lengths, int max_flows);

  //! @brief  Perform a more advanced simulation to generate QV predictors
  //! @param[in]  read.sequence         Base sequence
  //! @param[out] read.onemer_height    Expected 1-mer signal, used for scaling residuals
  //! @param[out] read.penalty_residual Absolute score of the called nuc hypothesis
  //! @param[out] read.penalty_mismatch Score difference to second-best nuc hypothesis
  void  ComputeQVmetrics(BasecallerRead& read); // Computes "oneMerHeight" and "deltaPenalty"

  //! @brief  Correct for uniform multiplicative scaling
  //! @param[in]  read.prediction               Model-predicted signal
  //! @param[in]  read.raw_measurements         Flow signal before normalization
  //! @param[out] read.normalized_measurements  Flow signal after normalization
  //! @param[in]  start_flow,end_flow           Range of flows to process
  float Normalize(BasecallerRead& read, int start_flow, int end_flow) const;

  //! @brief  Correct for flow-varying multiplicative and additive distortion
  //! @param[in]  read.prediction               Model-predicted signal
  //! @param[in]  read.raw_measurements         Flow signal before normalization
  //! @param[out] read.normalized_measurements  Flow signal after normalization
  //! @param[in]  num_steps                     Number of windows-worth of predictions to use
  //! @param[in]  window_size                   Size of a window in flows
  void  WindowedNormalize(BasecallerRead& read, int num_steps, int window_size) const;

  //! @brief    Use PID loop approach to correct for flow-varying gain and offset distortion
  //! @param[in]  read.prediction               Model-predicted signal
  //! @param[in]  read.raw_measurements         Flow signal before normalization
  //! @param[out] read.normalized_measurements  Flow signal after normalization
  //! @param[in]  num_samples                   Number of samples to correct
  void  PIDNormalize(BasecallerRead& read, const int num_samples);
  float PIDNormalize(BasecallerRead& read, const int start_flow, const int end_flow);

  //! @brief     Set pointers to recalibration model
  bool SetAsBs(const vector<vector< vector<float> > > *As, const vector<vector< vector<float> > > *Bs){
    As_ = As;
    Bs_ = Bs;
    pm_model_available_ = (As_ != NULL) and (Bs_ != NULL);
    recalibrate_predictions_ = pm_model_available_; // We bothered loading the model, of course we want to use it!
    return(pm_model_available_);
  };

  //! @brief     Enables the use of recalibration if a model is available
  bool EnableRecalibration() {
    recalibrate_predictions_ = pm_model_available_;
    return pm_model_available_;
  };

  //! @brief     Diasbles the use of recalibration and clears model information.
  void DisableRecalibration() {
    pm_model_available_ = false;
    recalibrate_predictions_ = false;
    As_ = 0; Bs_ = 0;
  };

  //! @brief    Treephaser's slot for partial base sequence, complete with tree search metrics and state for extending
  struct TreephaserPath {
    bool              in_use;                   //!< Is this slot in use?

    // Phasing and input-output state of this path
    int               flow;                     //!< In phase flow of last incorporated base
    vector<float>     state;                    //!< Histogram of flows at which last base was incorporated
    int               window_start;             //!< Start flow (inclusive) of meaningful state values
    int               window_end;               //!< End flow (noninclusive) of meaningful state values
    vector<float>     prediction;               //!< Model-based phased signal predicted for this path
    vector<char>      sequence;                 //!< Vector of ACGT bases corresponding to this path
    int               last_hp;                  //!< Length of the last homopolymer in sequence

    // Path metrics and related values
    float             path_metric;              //!< Primary tree search metric
    float             residual_left_of_window;  //!< Residual left of the state window
    float             per_flow_metric;          //!< Auxiliary tree search metric, useful for stack pruning
    int               dot_counter;              //!< Number of extreme mismatch flows encountered so far

    // Recalibration - we actually need 1 data structure to apply this model.
    vector<float>     calibA;                   //!< Multiplicative offset per inphase flow

    // PID loop state
    PIDloop           pidOffsetState;           //!< State of the pidOffset_ loop at window_start;
    PIDloop           pidGainState;             //!< State of the pidGain_ loop at window_start;
  };

  TreephaserPath& path(int idx) { return path_[idx]; }


  //! @brief  Set path to an empty sequence, a starting point for phasing simulation
  //! @param[out]  state    Path slot
  void InitializeState(TreephaserPath *state) const;

  //! @brief  Perform a path extension by one nucleotide
  //! @param[out]  child     Path slot to store the extended path
  //! @param[in]   parent    Path to be extended
  //! @param[in]   nuc       Nucleotide (integer) to extend the path by
  //! @param[in]   max_flow  Do not read/write past this flow
  void AdvanceState(TreephaserPath *child, const TreephaserPath *parent, char nuc, int max_flow) const;

  //! @brief  Perform a path extension by one nucleotide
  //! @param[in,out] state     Path to be extended in place
  //! @param[in]     nuc       Nucleotide (integer) to extend the path by
  //! @param[in]     max_flow  Do not read/write past this flow
  void AdvanceStateInPlace(TreephaserPath *state, char nuc, int max_flow) const;

  //! @brief  Switch to set state progression model
  //! @param[in]  diagonal_states : Sets attribute diagonal_states_
  void SetStateProgression(bool diagonal_states)
  { diagonal_states_ = diagonal_states; };


  //! @brief  Switch to disable / enable the use of recalibration during the normalization phase
  void SkipRecalDuringNormalization(bool skip_recal)
  { skip_recal_during_normalization_ = skip_recal; };


protected:

  void  ResetRecalibrationStructures();

  int                 windowSize_;                //!< Normalization window size

  double              my_cf_;                     //!< Stores the cf phasing parameter used to compute transitions
  double              my_ie_;                     //!< Stores the ie phasing parameter used to compute transitions
  double              my_dr_;                     //!< Stores the dr phasing parameter used to compute transitions
  ion::FlowOrder      flow_order_;                //!< Sequence of nucleotide flows
  vector<float>       transition_base_[8];        //!< Probability of polymerase incorporating and staying active
  vector<float>       transition_flow_[8];        //!< Probability of polymerase not incorporating and staying active
  vector<TreephaserPath> path_;                   //!< Preallocated space for partial path slots


  const static int    kNumPaths = 8;              //!< Maximum number of paths considered
  const static float  kExtendThreshold = 0.2;     //!< Threshold for extending paths
  const static float  kNegativeMultiplier = 2.0;  //!< Extra weight on the negative residuals
  const static float  kDotThreshold = 0.3;        //!< percentage of expected Signal that constitutes a "dot"
  const static int    kMaxHP = MAX_HPXLEN;        //!< Maximum callable homopolymer length
  const static float  kStateWindowCutoff = 1e-6;  //!< Minimum fraction to be taken into account
  const static int    kMaxPathDelay = 40;         //!< Paths that are delayed more are killed

  // PID loop and coefficients
  PIDloop             pidOffset_;
  PIDloop             pidGain_;

  const static float  kPgainO       = 0.06f;//0.075f;
  const static float  kIgainO       = 0.005f;
  const static float  kDgainO       = 0.0f;
  const static float  kInitOffset   = 0.0f;
  const static float  kPgainG       = 0.06f;//0.075f;
  const static float  kIgainG       = 0.005f;
  const static float  kDgainG       = 0.0f;
  const static float  kInitGain     = 1.0f;

  const vector< vector< vector<float> > > *As_; //!< Pointer to recalibration structure: multiplicative constant
  const vector< vector< vector<float> > > *Bs_; //!< Pointer to recalibration structure: additive constant
  bool pm_model_available_;                     //!< Signals availability of a recalibration model
  bool recalibrate_predictions_;                //!< Switch to use recalibration model during metric generation
  bool skip_recal_during_normalization_;        //!< Switch to skip recalibration during the normalization phase
  bool diagonal_states_;                     //!< Turn on a diagonalized state model

};

#endif // DPTREEPHASER_H
