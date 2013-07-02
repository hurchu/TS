/* Copyright (C) 2012 Ion Torrent Systems, Inc. All Rights Reserved */

#include "TreephaserSSE.h"

#include <vector>
#include <string>
#include <algorithm>
#include <math.h>
#include <x86intrin.h>
#include <cstring>
#include <cstdio>
#include <cassert>


using namespace std;

#define SHUF_PS(reg, mode) _mm_castsi128_ps(_mm_shuffle_epi32(_mm_castps_si128(reg), mode))

#define AD_STATE_OFS (0*MAX_VALS*4*sizeof(float)+16)
#define AD_PRED_OFS (1*MAX_VALS*4*sizeof(float)+16)
#define AD_NRES_OFS (2*MAX_VALS*4*sizeof(float)+16)
#define AD_PRES_OFS (3*MAX_VALS*4*sizeof(float)+16)

#define NO_SSE_MESSAGE "TreephaserSSE compiled without SSE"
#define NO_SSE3_MESSAGE "TreephaserSSE compiled without SSE3"

namespace {

ALWAYS_INLINE float Sqr(float val) {
  return val*val;
}

inline void setZeroSSE(void *dst, int size) {
#ifdef __SSE__
  __m128i r0 = _mm_setzero_si128();
  while((size & 31) != 0) {
    --size;
    ((char RESTRICT_PTR)dst)[size] = char(0);
  }
  while(size > 0) {
    _mm_store_si128((__m128i RESTRICT_PTR)((char RESTRICT_PTR)dst+size-16), r0);
    _mm_store_si128((__m128i RESTRICT_PTR)((char RESTRICT_PTR)dst+size-32), r0);
    size -= 32;
  }
#else
#pragma message NO_SSE_MESSAGE
#endif
}

inline void setValueSSE(float *buf, float val, int size) {
#ifdef __SSE__
  int mod = size % 4;
  int i=0;
  while(i<(size - mod)) {
    *((__m128 RESTRICT_PTR)(buf + i)) = _mm_set1_ps(val);
    i+=4;
  }
  
  // fill the rest of the buffer
  while (i<size) {
    buf[i] = val;
    i++;
  }  
#else
#pragma message NO_SSE_MESSAGE
#endif
}


inline void copySSE(void *dst, void *src, int size) {
#ifdef __SSE__
  while((size & 31) != 0) {
    --size;
    ((char RESTRICT_PTR)dst)[size] = ((char RESTRICT_PTR)src)[size];
  }
  while(size > 0) {
    __m128i r0 = _mm_load_si128((__m128i RESTRICT_PTR)((char RESTRICT_PTR)src+size-16));
    __m128i r1 = _mm_load_si128((__m128i RESTRICT_PTR)((char RESTRICT_PTR)src+size-32));
    _mm_store_si128((__m128i RESTRICT_PTR)((char RESTRICT_PTR)dst+size-16), r0);
    _mm_store_si128((__m128i RESTRICT_PTR)((char RESTRICT_PTR)dst+size-32), r1);
    size -= 32;
  }
#else
#pragma message NO_SSE_MESSAGE
#endif
}

inline float sumOfSquaredDiffsFloatSSE(float RESTRICT_PTR src1, float RESTRICT_PTR src2, int count) {
  float sum = 0.0f;
#ifdef __SSE__
  while((count & 3) != 0) {
    --count;
    sum += Sqr(src1[count]-src2[count]);
  }
  __m128 r0 = _mm_load_ss(&sum);
  while(count > 0) {
    __m128 r1 = _mm_load_ps(&src1[count-4]);
    r1 = _mm_sub_ps(r1, *((__m128 RESTRICT_PTR)(&src2[count-4])));
    count -= 4;
    r1 = _mm_mul_ps(r1, r1);
    r0 = _mm_add_ps(r0, r1);
  }
  __m128 r2 = r0;
  r0 = _mm_movehl_ps(r0, r0);
  r0 = _mm_add_ps(r0, r2);
  r0 = _mm_unpacklo_ps(r0, r0);
  r2 = r0;
  r0 = _mm_movehl_ps(r0, r0);
  r0 = _mm_add_ps(r0, r2);
  _mm_store_ss(&sum, r0);
#else
#pragma message NO_SSE_MESSAGE
#endif
  return sum;
}

inline float vecSumSSE(float RESTRICT_PTR src, int count){
#ifdef __SSE3__
  float sum = 0.0f;
  while((count & 3) != 0) {
    --count;
    sum += src[count];
  }
  __m128 r0 = _mm_load_ss(&sum);
  while(count > 0) {
    __m128 r1 = _mm_load_ps(&src[count-4]);
    count -= 4;
    r0 = _mm_add_ps(r0, r1);
  }
  r0 = _mm_hadd_ps(r0, r0);
  r0 = _mm_hadd_ps(r0, r0);
  return _mm_cvtss_f32(r0);
#else
#pragma message NO_SSE3_MESSAGE
  return 0.0f;
#endif
}

inline float  sumOfSquaredDiffsFloatSSE_recal(float RESTRICT_PTR src1, float RESTRICT_PTR src2, float RESTRICT_PTR A, float RESTRICT_PTR B, int count) {
  //src2 is prediction
  //A and B are recal_model coefficients
  float sum = 0.0f;
  while((count & 3) != 0) {
    --count;
      sum += Sqr(src1[count]-src2[count]*A[count]-B[count]);
  }
#ifdef __SSE__
  __m128 r0 = _mm_load_ss(&sum);
  while(count > 0) {
    __m128 r1 = _mm_load_ps(&src1[count-4]);
    __m128 rp = _mm_load_ps(&src2[count-4]);
    __m128 coeff_a = _mm_load_ps(&A[count-4]);
    __m128 coeff_b = _mm_load_ps(&B[count-4]);
    rp = _mm_mul_ps(rp, coeff_a);
    rp = _mm_add_ps(rp, coeff_b);
    r1 = _mm_sub_ps(r1, rp);
    count -= 4;
    r1 = _mm_mul_ps(r1, r1);
    r0 = _mm_add_ps(r0, r1);
  }
  __m128 r2 = r0;
  r0 = _mm_movehl_ps(r0, r0);
  r0 = _mm_add_ps(r0, r2);
  r0 = _mm_unpacklo_ps(r0, r0);
  r2 = r0;
  r0 = _mm_movehl_ps(r0, r0);
  r0 = _mm_add_ps(r0, r2);
  _mm_store_ss(&sum, r0);
#else
#pragma message NO_SSE_MESSAGE
#endif
  return sum;
}

inline void sumVectFloatSSE(float RESTRICT_PTR dst, float RESTRICT_PTR src, int count) {
#ifdef __SSE__
  while((count & 3) != 0) {
    --count;
    dst[count] += src[count];
  }
  while(count > 0) {
    __m128 r0 = _mm_load_ps(&dst[count-4]);
    r0 = _mm_add_ps(r0, *((__m128 RESTRICT_PTR)(&src[count-4])));
    _mm_store_ps(&dst[count-4], r0);
    count -= 4;
  }
#else
#pragma message NO_SSE_MESSAGE
#endif
}

inline __m128 applyRecalModel(__m128 current_value, PathRec RESTRICT_PTR current_path, int i){
    __m128 rCoeffA = _mm_set1_ps(current_path->calib_A[i]);
    __m128 rCoeffB = _mm_set1_ps(current_path->calib_B[i]);
    current_value = _mm_mul_ps(current_value, rCoeffA);
    current_value = _mm_add_ps(current_value, rCoeffB);
    return current_value;
}

};


// ----------------------------------------------------------------------------

// Constructor used in variant caller
TreephaserSSE::TreephaserSSE()
{
  flow_order_.SetFlowOrder("TACG", 4);
  SetFlowOrder(flow_order_, 38);
}

// Constructor used in Basecaller
TreephaserSSE::TreephaserSSE(const ion::FlowOrder& flow_order, const int windowSize)
{
	SetFlowOrder(flow_order, windowSize);
}

// Initialize Object
void TreephaserSSE::SetFlowOrder(const ion::FlowOrder& flow_order, const int windowSize)
{
  SetNormalizationWindowSize(windowSize);
  flow_order_ = flow_order;
  num_flows_ = flow_order.num_flows();

  // For some perverse reason cppcheck does not like this loop
  //for (int path = 0; path <= MAX_PATHS; ++path)
  //  sv_PathPtr[path] = &(sv_pathBuf[path]);
  sv_PathPtr[0] = &(sv_pathBuf[0]);
  sv_PathPtr[1] = &(sv_pathBuf[1]);
  sv_PathPtr[2] = &(sv_pathBuf[2]);
  sv_PathPtr[3] = &(sv_pathBuf[3]);
  sv_PathPtr[4] = &(sv_pathBuf[4]);
  sv_PathPtr[5] = &(sv_pathBuf[5]);
  sv_PathPtr[6] = &(sv_pathBuf[6]);
  sv_PathPtr[7] = &(sv_pathBuf[7]);
  sv_PathPtr[8] = &(sv_pathBuf[8]);

  ad_MinFrac[0] = ad_MinFrac[1] = ad_MinFrac[2] = ad_MinFrac[3] = 1e-6f;

  int nextIdx[4];
  nextIdx[3] = nextIdx[2] = nextIdx[1] = nextIdx[0] = short(num_flows_);
  for(int flow = num_flows_-1; flow >= 0; --flow) {
    nextIdx[flow_order_.int_at(flow)] = flow;
    ts_NextNuc[0][flow] = (short)(ts_NextNuc4[flow][0] = nextIdx[0]);
    ts_NextNuc[1][flow] = (short)(ts_NextNuc4[flow][1] = nextIdx[1]);
    ts_NextNuc[2][flow] = (short)(ts_NextNuc4[flow][2] = nextIdx[2]);
    ts_NextNuc[3][flow] = (short)(ts_NextNuc4[flow][3] = nextIdx[3]);
  }

  ts_StepCnt = 0;
  for(int i = windowSize_ << 1; i < num_flows_; i += windowSize_) {
    ts_StepBeg[ts_StepCnt] = (ts_StepEnd[ts_StepCnt] = i)-(windowSize_ << 1);
    ts_StepCnt++;
  }
  ts_StepBeg[ts_StepCnt] = (ts_StepEnd[ts_StepCnt] = num_flows_)-(windowSize_ << 1);
  ts_StepEnd[++ts_StepCnt] = num_flows_;
  ts_StepBeg[ts_StepCnt] = 0;

  for (int p = 0; p <= 8; ++p) {
      std::fill(sv_PathPtr[p]->calib_A, sv_PathPtr[p]->calib_A+MAX_VALS,1);
      memset(sv_PathPtr[p]->calib_B, 0, sizeof(float)*MAX_VALS);
  }

  pm_model_available_ = false;
  pm_model_enabled_ = false;

}


void TreephaserSSE::SetModelParameters(double cf, double ie)
{
  double dist[4] = { 0.0, 0.0, 0.0, 0.0 };

  for(int flow = 0; flow < num_flows_; ++flow) {
    dist[flow_order_.int_at(flow)] = 1.0;
    ts_Transition4[flow][0] = ts_Transition[0][flow] = float(dist[0]*(1-ie));
    dist[0] *= cf;
    ts_Transition4[flow][1] = ts_Transition[1][flow] = float(dist[1]*(1-ie));
    dist[1] *= cf;
    ts_Transition4[flow][2] = ts_Transition[2][flow] = float(dist[2]*(1-ie));
    dist[2] *= cf;
    ts_Transition4[flow][3] = ts_Transition[3][flow] = float(dist[3]*(1-ie));
    dist[3] *= cf;
  }
}



void TreephaserSSE::NormalizeAndSolve(BasecallerRead& read)
{
  copySSE(rd_NormMeasure, &read.raw_measurements[0], num_flows_*sizeof(float));
  for(int step = 0; step < ts_StepCnt; ++step) {
    bool is_final = Solve(ts_StepBeg[step], ts_StepEnd[step]);
    WindowedNormalize(read, step);
    if (is_final)
      break;
  }
  if(pm_model_available_) pm_model_enabled_ = true;  
  //final stage of solve and calculate the state_inphase for QV prediction
  state_inphase_enabled = true;

  Solve(ts_StepBeg[ts_StepCnt], ts_StepEnd[ts_StepCnt]);

  read.sequence.resize(sv_PathPtr[MAX_PATHS]->sequence_length);
  copySSE(&read.sequence[0], sv_PathPtr[MAX_PATHS]->sequence, sv_PathPtr[MAX_PATHS]->sequence_length*sizeof(char));
  copySSE(&read.normalized_measurements[0], rd_NormMeasure, num_flows_*sizeof(float));
  setZeroSSE(&read.prediction[0], num_flows_*sizeof(float));
  copySSE(&read.prediction[0], sv_PathPtr[MAX_PATHS]->pred, sv_PathPtr[MAX_PATHS]->window_end*sizeof(float));
  setZeroSSE(&read.state_inphase[0], num_flows_*sizeof(float));
  copySSE(&read.state_inphase[0], sv_PathPtr[MAX_PATHS]->state_inphase, sv_PathPtr[MAX_PATHS]->window_end*sizeof(float));

  //reset to 1 and zero for calib_A and calib_B as window_end will be larger than flow
  if(pm_model_enabled_){
    for (int p = 0; p <= 8; ++p) {
      setValueSSE(&(sv_PathPtr[p]->calib_A[0]), 1.0f, num_flows_);
      setZeroSSE(&(sv_PathPtr[p]->calib_B[0]), num_flows_ << 2);
    }
  }

  //reset state_inphase
  pm_model_enabled_ = false;
  if(state_inphase_enabled){
    for (int p = 0; p <= 8; ++p) {
      setZeroSSE(&(sv_PathPtr[p]->state_inphase[0]), num_flows_ << 2);
    }
  }
  state_inphase_enabled = false;
}



// nextState is only used for the simulation step.

void TreephaserSSE::nextState(PathRec RESTRICT_PTR path, int nuc, int end) {
  int idx = ts_NextNuc[nuc][path->flow];
  if(idx > end)
    idx = end;
  if(path->flow != idx) {
    path->flow = idx;
    idx = path->window_end;
    float alive = 0.0f;
    float RESTRICT_PTR trans = ts_Transition[nuc];
    const float minFrac = 1e-6f;
    int b = path->window_start;
    int e = idx--;
    int i = b;
    while(i < idx) {
      alive += path->state[i];
      float s = alive * trans[i];
      path->state[i] = s;
      alive -= s;
      ++i;
      if(!(s < minFrac))
        break;
      b++;
    }
    if(i > b) {
      while(i < idx) {
        alive += path->state[i];
        float s = alive * trans[i];
        path->state[i] = s;
        alive -= s;
        ++i;
      }
      alive += path->state[i];
      while(i < end) {
        float s = alive * trans[i];
        path->state[i] = s;
        alive -= s;
        if(!(alive > minFrac))
          break;
        path->pred[++i] = 0.0f;
        e++;
      }
    } else {
      alive += path->state[i];
      while(i < e) {
        float s = alive * trans[i];
        path->state[i] = s;
        alive -= s;
        if(i++ == b)
          if((i < e) && (s < minFrac))
            b++;
        if((i == e) && (e < end) && (alive > minFrac))
          path->pred[e++] = 0.0f;
      }
    }
    path->window_start = b;
    path->window_end = e;
  }
}


void TreephaserSSE::advanceState4(PathRec RESTRICT_PTR parent, int end)
{
#ifdef __SSE__
  int idx = parent->flow;

  // max flows
  __m128i rFlowEnd = _mm_cvtsi32_si128(end);
  __m128i rNucCpy = _mm_cvtsi32_si128(idx);

  // child flows or the flow at which child nuc incorporates (corresponds to find child flow in AdvanceState()
  // in DPTreephaser.cpp
  __m128i rNucIdx = _mm_load_si128((__m128i RESTRICT_PTR)(ts_NextNuc4[idx]));
  rFlowEnd = _mm_shuffle_epi32(rFlowEnd, _MM_SHUFFLE(0, 0, 0, 0));
  rNucCpy = _mm_shuffle_epi32(rNucCpy, _MM_SHUFFLE(0, 0, 0, 0));
  rNucIdx = _mm_min_epi16(rNucIdx, rFlowEnd);

  // compare parent flow and child flows 
  rNucCpy = _mm_cmpeq_epi32(rNucCpy, rNucIdx);

  _mm_store_si128((__m128i RESTRICT_PTR)ad_FlowEnd, rFlowEnd);

  // four child flows in four 32 bit integers
  _mm_store_si128((__m128i RESTRICT_PTR)ad_Idx, rNucIdx);

  // convert ints to floats
  __m128 rParNuc = _mm_castsi128_ps(rNucCpy);

  __m128 rAlive = _mm_setzero_ps();

  // penalties for each nuc corresponding to four childs
  __m128 rPenNeg = rAlive;
  __m128 rPenPos = rAlive;

  int parLast = parent->window_end;
  __m128i rEnd = _mm_cvtsi32_si128(parLast--);
  __m128i rBeg = _mm_cvtsi32_si128(parent->window_start);
  rEnd = _mm_shuffle_epi32(rEnd, _MM_SHUFFLE(0, 0, 0, 0));
  rBeg = _mm_shuffle_epi32(rBeg, _MM_SHUFFLE(0, 0, 0, 0));


  int i = parent->window_start;
  int j = 0;
  ad_Adv = 1;

  // iterate over the flows from parent->window_start to (parent->window_end - 1)
  while(i < parLast) {

    __m128 rS = _mm_load_ss(&parent->state[i]);
    __m128i rI = _mm_cvtsi32_si128(i);
    rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));

    // tracking flow from parent->window_start
    rI = _mm_shuffle_epi32(rI, _MM_SHUFFLE(0, 0, 0, 0));

    // add parent state at this flow
    rAlive = _mm_add_ps(rAlive, rS);

    // one of the entries is  where the homopolymer is extended, rest are 0
    __m128 rTemp1s = rParNuc;

    // keep the parent state for child  where parent homopolymer is extended, rest are 0
    rS = _mm_and_ps(rS, rTemp1s);

    // select transitions where this nuc begins a new homopolymer
    rTemp1s = _mm_andnot_ps(rTemp1s, *((__m128 RESTRICT_PTR)(ts_Transition4[i])));

    // multiply transition probabilities with alive 
    rTemp1s = _mm_mul_ps(rTemp1s, rAlive);

    // child state for this flow
    rS = _mm_add_ps(rS, rTemp1s);

    // storing child states to the buffer
    _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_STATE_OFS])), rS);

    rAlive = _mm_sub_ps(rAlive, rS);

    __m128i rTemp1i = rBeg;

    // obtain window start for child which doesn't extend parent homopolymer. The one that extends 
    // has all bits for its word as 1
    rTemp1i = _mm_or_si128(rTemp1i, _mm_castps_si128(rParNuc));
   
    // compare parent window start to current flow i. All match except one where parent last hp extends
    rTemp1i = _mm_cmpeq_epi32(rTemp1i, rI);

    // filter min frac for nuc homopolymer child paths
    rTemp1s = _mm_and_ps(_mm_castsi128_ps(rTemp1i), *((__m128 RESTRICT_PTR)ad_MinFrac));

    // compares not less than equal to for two _m128i words
    rTemp1s = _mm_cmpnle_ps(rTemp1s, rS);

    // increasing child window start if child state less than state window cut off.         
    rBeg = _mm_sub_epi32(rBeg, _mm_castps_si128(rTemp1s));

    // this intrinsic gives sign of each word in binary indicating 1 for -ve sign and 0 for +ve
    // if ad_adv is greater than 0, it indicates increase in child window start for some child path
    ad_Adv = _mm_movemask_ps(rTemp1s);

    // load parent prediction
    rTemp1s = _mm_load_ss(&parent->pred[i]);
    rTemp1s = SHUF_PS(rTemp1s, _MM_SHUFFLE(0, 0, 0, 0));

    // add child state to parent prediction
    rTemp1s = _mm_add_ps(rTemp1s, rS);

    // storing child predictions
    _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRED_OFS])), rTemp1s);

    // apply recalibration model paramters to predicted signal if model is available
    if(pm_model_enabled_ && parent->calib_B[i]){
        rTemp1s = applyRecalModel(rTemp1s, parent, i);
    }

    // load normalized measurement for the parent
    rS = _mm_load_ss(&rd_NormMeasure[i]);
    rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));

    // residual from normalized and predicted values for this flow
    rS = _mm_sub_ps(rS, rTemp1s);

    rTemp1s = rS;

    // find out the negative residual. The number which are -ve have highest bit one and therefore gives
    // four ints with 0's in the ones which are not negative
    rS = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(rS),31));

    // squared residual
    rTemp1s = _mm_mul_ps(rTemp1s, rTemp1s);

    // select negative residuals
    rS = _mm_and_ps(rS, rTemp1s);

    // select positive residuals
    rTemp1s = _mm_xor_ps(rTemp1s, rS);

    // add to negative penalty the square of negative residuals
    rPenNeg = _mm_add_ps(rPenNeg, rS);

    // add squared residuals to postive penalty
    rPenPos = _mm_add_ps(rPenPos, rTemp1s);

    // running sum of negative penalties
    _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_NRES_OFS])), rPenNeg);

    // running sum of positive penalties
    _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRES_OFS])), rPenPos);

    ++i;
    j += 4;
    if(ad_Adv == 0)
      break;
  }

  // if none of the child paths has increase in window start
  if(EXPECTED(ad_Adv == 0)) {

    _mm_store_si128((__m128i RESTRICT_PTR)ad_Beg, rBeg);

    while(i < parLast) {
      __m128 rS = _mm_load_ss(&parent->state[i]);
      rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));

      rAlive = _mm_add_ps(rAlive, rS);

      __m128 rTemp1s = rParNuc;
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_andnot_ps(rTemp1s, *((__m128 RESTRICT_PTR)(ts_Transition4[i])));
      rTemp1s = _mm_mul_ps(rTemp1s, rAlive);
      rS = _mm_add_ps(rS, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_STATE_OFS])), rS);

      rAlive = _mm_sub_ps(rAlive, rS);

      rTemp1s = _mm_load_ss(&parent->pred[i]);
      rTemp1s = SHUF_PS(rTemp1s, _MM_SHUFFLE(0, 0, 0, 0));
      rTemp1s = _mm_add_ps(rTemp1s, rS);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRED_OFS])), rTemp1s);

      if(pm_model_enabled_ && parent->calib_B[i]){ //disabling this makes difference
          rTemp1s = applyRecalModel(rTemp1s, parent, i);
       }

      rS = _mm_load_ss(&rd_NormMeasure[i]);
      rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));
      rS = _mm_sub_ps(rS, rTemp1s);

      rTemp1s = rS;
      rS = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(rS),31));
      rTemp1s = _mm_mul_ps(rTemp1s, rTemp1s);
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_xor_ps(rTemp1s, rS);
      rPenNeg = _mm_add_ps(rPenNeg, rS);
      rPenPos = _mm_add_ps(rPenPos, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_NRES_OFS])), rPenNeg);
      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRES_OFS])), rPenPos);

      ++i;
      j += 4;
    }

    // flow == parent->window_end - 1
    {
      __m128 rS = _mm_load_ss(&parent->state[i]);
      __m128i rI = _mm_cvtsi32_si128(i);
      rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));
      rI = _mm_shuffle_epi32(rI, _MM_SHUFFLE(0, 0, 0, 0));

      rAlive = _mm_add_ps(rAlive, rS);

      __m128 rTemp1s = rParNuc;
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_andnot_ps(rTemp1s, *((__m128 RESTRICT_PTR)(ts_Transition4[i])));
      rTemp1s = _mm_mul_ps(rTemp1s, rAlive);
      rS = _mm_add_ps(rS, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_STATE_OFS])), rS);

      rAlive = _mm_sub_ps(rAlive, rS);

      rTemp1s = _mm_load_ss(&parent->pred[i]);
      rTemp1s = SHUF_PS(rTemp1s, _MM_SHUFFLE(0, 0, 0, 0));
      rTemp1s = _mm_add_ps(rTemp1s, rS);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRED_OFS])), rTemp1s);  

      if(pm_model_enabled_ && parent->calib_B[i]){
          rTemp1s = applyRecalModel(rTemp1s, parent, i);
      }

      rS = _mm_load_ss(&rd_NormMeasure[i]);
      rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));
      rS = _mm_sub_ps(rS, rTemp1s);

      rTemp1s = rS;
      rS = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(rS),31));
      rTemp1s = _mm_mul_ps(rTemp1s, rTemp1s);
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_xor_ps(rTemp1s, rS);
      rPenNeg = _mm_add_ps(rPenNeg, rS);
      rPenPos = _mm_add_ps(rPenPos, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_NRES_OFS])), rPenNeg);
      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRES_OFS])), rPenPos);

      rTemp1s = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_castps_si128(rTemp1s), _mm_castps_si128(rTemp1s)));
      rTemp1s = _mm_castsi128_ps(_mm_add_epi32(_mm_castps_si128(rTemp1s), rEnd));
      rTemp1s = _mm_or_ps(rTemp1s, rParNuc);
      rTemp1s = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_castps_si128(rTemp1s), rI));
      rTemp1s = _mm_and_ps(rTemp1s, rAlive);
      rTemp1s = _mm_cmpnle_ps(rTemp1s, *((__m128 RESTRICT_PTR)ad_MinFrac));
      
      // if non zero than an increase in window end for some child paths
      ad_Adv = _mm_movemask_ps(rTemp1s);
      rEnd = _mm_sub_epi32(rEnd, _mm_castps_si128(rTemp1s));

      ++i;
      j += 4;
    }

   // flow greater than parent window end
    while((i < end) && (ad_Adv != 0)) {

      __m128 rS = _mm_load_ss(&parent->state[i]);
      __m128i rI = _mm_cvtsi32_si128(i);
      rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));
      rI = _mm_shuffle_epi32(rI, _MM_SHUFFLE(0, 0, 0, 0));

      __m128 rTemp1s = rParNuc;
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_andnot_ps(rTemp1s, *((__m128 RESTRICT_PTR)(ts_Transition4[i])));
      rTemp1s = _mm_mul_ps(rTemp1s, rAlive);
      rS = _mm_add_ps(rS, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_STATE_OFS])), rS);

      rAlive = _mm_sub_ps(rAlive, rS);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRED_OFS])), rS);

      if(pm_model_enabled_ && parent->calib_B[i]){
           rS = applyRecalModel(rS, parent, i);
      }

      rTemp1s = _mm_load_ss(&rd_NormMeasure[i]);
      rTemp1s = SHUF_PS(rTemp1s, _MM_SHUFFLE(0, 0, 0, 0));
      rTemp1s = _mm_sub_ps(rTemp1s, rS);
      rS = rTemp1s;

      rS = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(rS),31));
      rTemp1s = _mm_mul_ps(rTemp1s, rTemp1s);
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_xor_ps(rTemp1s, rS);
      rPenNeg = _mm_add_ps(rPenNeg, rS);
      rPenPos = _mm_add_ps(rPenPos, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_NRES_OFS])), rPenNeg);
      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRES_OFS])), rPenPos);

      rTemp1s = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_castps_si128(rTemp1s), _mm_castps_si128(rTemp1s)));
      rTemp1s = _mm_castsi128_ps(_mm_add_epi32(_mm_castps_si128(rTemp1s), rEnd));
      rTemp1s = _mm_or_ps(rTemp1s, rParNuc);
      rTemp1s = _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_castps_si128(rTemp1s), rI));
      rTemp1s = _mm_and_ps(rTemp1s, rAlive);
      rTemp1s = _mm_cmpnle_ps(rTemp1s, *((__m128 RESTRICT_PTR)ad_MinFrac));
      ad_Adv = _mm_movemask_ps(rTemp1s);
      rEnd = _mm_sub_epi32(rEnd, _mm_castps_si128(rTemp1s));

      ++i;
      j += 4;
    }

    rEnd = _mm_min_epi16(rEnd, *((__m128i RESTRICT_PTR)ad_FlowEnd));
    _mm_store_si128((__m128i RESTRICT_PTR)ad_End, rEnd);

  } 
  // flow = parent->window_end 
  else {

    {
      __m128 rS = _mm_load_ss(&parent->state[i]);
      __m128i rI = _mm_cvtsi32_si128(i);
      rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));
      rI = _mm_shuffle_epi32(rI, _MM_SHUFFLE(0, 0, 0, 0));

      rAlive = _mm_add_ps(rAlive, rS);

      __m128 rTemp1s = rParNuc;
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_andnot_ps(rTemp1s, *((__m128 RESTRICT_PTR)(ts_Transition4[i])));
      rTemp1s = _mm_mul_ps(rTemp1s, rAlive);
      rS = _mm_add_ps(rS, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_STATE_OFS])), rS);

      rAlive = _mm_sub_ps(rAlive, rS);

      __m128i rTemp1i = rBeg;
      rTemp1i = _mm_or_si128(rTemp1i, _mm_castps_si128(rParNuc));
      rTemp1i = _mm_cmpeq_epi32(rTemp1i, rI);
      rTemp1s = _mm_and_ps(_mm_castsi128_ps(rTemp1i), *((__m128 RESTRICT_PTR)ad_MinFrac));
      rTemp1s = _mm_cmpnle_ps(rTemp1s, rS);
      rBeg = _mm_sub_epi32(rBeg, _mm_castps_si128(rTemp1s));
      rTemp1i = rBeg;
      rTemp1i = _mm_cmpeq_epi32(rTemp1i, rEnd);
      rBeg = _mm_add_epi32(rBeg, rTemp1i);

      rTemp1s = _mm_load_ss(&parent->pred[i]);
      rTemp1s = SHUF_PS(rTemp1s, _MM_SHUFFLE(0, 0, 0, 0));
      rTemp1s = _mm_add_ps(rTemp1s, rS);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRED_OFS])), rTemp1s);

      if(pm_model_enabled_ && parent->calib_B[i]){
           rTemp1s = applyRecalModel(rTemp1s, parent, i);
      }

      rS = _mm_load_ss(&rd_NormMeasure[i]);
      rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));
      rS = _mm_sub_ps(rS, rTemp1s);

      rTemp1s = rS;
      rS = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(rS),31));
      rTemp1s = _mm_mul_ps(rTemp1s, rTemp1s);
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_xor_ps(rTemp1s, rS);
      rPenNeg = _mm_add_ps(rPenNeg, rS);
      rPenPos = _mm_add_ps(rPenPos, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_NRES_OFS])), rPenNeg);
      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRES_OFS])), rPenPos);

      rTemp1i = _mm_cmpeq_epi32(rTemp1i, rTemp1i);
      rTemp1i = _mm_add_epi32(rTemp1i, rEnd);
      rTemp1i = _mm_or_si128(rTemp1i, _mm_castps_si128(rParNuc));
      rTemp1i = _mm_cmpeq_epi32(rTemp1i, rI);
      rTemp1s = _mm_and_ps(_mm_castsi128_ps(rTemp1i), rAlive);
      rTemp1s = _mm_cmpnle_ps(rTemp1s, *((__m128 RESTRICT_PTR)ad_MinFrac));
      ad_Adv = _mm_movemask_ps(rTemp1s);
      rEnd = _mm_sub_epi32(rEnd, _mm_castps_si128(rTemp1s));

      ++i;
      j += 4;
    }

    // child->window_end > parent->window_end
    while((i < end) && (ad_Adv != 0)) {

      __m128 rS = _mm_load_ss(&parent->state[i]);
      __m128i rI = _mm_cvtsi32_si128(i);
      rS = SHUF_PS(rS, _MM_SHUFFLE(0, 0, 0, 0));
      rI = _mm_shuffle_epi32(rI, _MM_SHUFFLE(0, 0, 0, 0));

      __m128 rTemp1s = rParNuc;
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_andnot_ps(rTemp1s, *((__m128 RESTRICT_PTR)(ts_Transition4[i])));
      rTemp1s = _mm_mul_ps(rTemp1s, rAlive);
      rS = _mm_add_ps(rS, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_STATE_OFS])), rS);

      rAlive = _mm_sub_ps(rAlive, rS);

      __m128i rTemp1i = rBeg;
      rTemp1i = _mm_or_si128(rTemp1i, _mm_castps_si128(rParNuc));
      rTemp1i = _mm_cmpeq_epi32(rTemp1i, rI);
      rTemp1s = _mm_and_ps(_mm_castsi128_ps(rTemp1i), *((__m128 RESTRICT_PTR)ad_MinFrac));
      rTemp1s = _mm_cmpnle_ps(rTemp1s, rS);
      rBeg = _mm_sub_epi32(rBeg, _mm_castps_si128(rTemp1s));
      rTemp1i = rBeg;
      rTemp1i = _mm_cmpeq_epi32(rTemp1i, rEnd);
      rBeg = _mm_add_epi32(rBeg, rTemp1i);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRED_OFS])), rS);

      if(pm_model_enabled_ && parent->calib_B[i]){
           rS = applyRecalModel(rS, parent, i);
      }

      rTemp1s = _mm_load_ss(&rd_NormMeasure[i]);
      rTemp1s = SHUF_PS(rTemp1s, _MM_SHUFFLE(0, 0, 0, 0));
      rTemp1s = _mm_sub_ps(rTemp1s, rS);
      rS = rTemp1s;

      rS = _mm_castsi128_ps(_mm_srai_epi32(_mm_castps_si128(rS),31));
      rTemp1s = _mm_mul_ps(rTemp1s, rTemp1s);
      rS = _mm_and_ps(rS, rTemp1s);
      rTemp1s = _mm_xor_ps(rTemp1s, rS);
      rPenNeg = _mm_add_ps(rPenNeg, rS);
      rPenPos = _mm_add_ps(rPenPos, rTemp1s);

      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_NRES_OFS])), rPenNeg);
      _mm_store_ps((float RESTRICT_PTR)(&(ad_Buf[j*4+AD_PRES_OFS])), rPenPos);

      rTemp1i = _mm_cmpeq_epi32(rTemp1i, rTemp1i);
      rTemp1i = _mm_add_epi32(rTemp1i, rEnd);
      rTemp1i = _mm_or_si128(rTemp1i, _mm_castps_si128(rParNuc));
      rTemp1i = _mm_cmpeq_epi32(rTemp1i, rI);
      rTemp1s = _mm_and_ps(_mm_castsi128_ps(rTemp1i), rAlive);
      rTemp1s = _mm_cmpnle_ps(rTemp1s, *((__m128 RESTRICT_PTR)ad_MinFrac));
      ad_Adv = _mm_movemask_ps(rTemp1s);
      rEnd = _mm_sub_epi32(rEnd, _mm_castps_si128(rTemp1s));

      ++i;
      j += 4;
    }

    rEnd = _mm_min_epi16(rEnd, *((__m128i RESTRICT_PTR)ad_FlowEnd));
    _mm_store_si128((__m128i RESTRICT_PTR)ad_Beg, rBeg);
    _mm_store_si128((__m128i RESTRICT_PTR)ad_End, rEnd);

  }
#else
#pragma message NO_SSE_MESSAGE
#endif
}

void TreephaserSSE::sumNormMeasures() {
  int i = num_flows_;
  float sum = 0.0f;
  rd_SqNormMeasureSum[i] = 0.0f;
  while(--i >= 0)
    rd_SqNormMeasureSum[i] = (sum += rd_NormMeasure[i]*rd_NormMeasure[i]);
}

// -------------------------------------------------

void TreephaserSSE::SolveRead(BasecallerRead& read, int begin_flow, int end_flow)
{
  copySSE(rd_NormMeasure, &read.normalized_measurements[0], num_flows_*sizeof(float));
  setZeroSSE(sv_PathPtr[MAX_PATHS]->pred, num_flows_*sizeof(float));
  copySSE(sv_PathPtr[MAX_PATHS]->sequence, &read.sequence[0], (int)read.sequence.size()*sizeof(char));
  sv_PathPtr[MAX_PATHS]->sequence_length = read.sequence.size();

  Solve(begin_flow, end_flow);

  read.sequence.resize(sv_PathPtr[MAX_PATHS]->sequence_length);
  copySSE(&read.sequence[0], sv_PathPtr[MAX_PATHS]->sequence, sv_PathPtr[MAX_PATHS]->sequence_length*sizeof(char));
  copySSE(&read.normalized_measurements[0], rd_NormMeasure, num_flows_*sizeof(float));
  setZeroSSE(&read.prediction[0], num_flows_*sizeof(float));
  copySSE(&read.prediction[0], sv_PathPtr[MAX_PATHS]->pred, sv_PathPtr[MAX_PATHS]->window_end*sizeof(float));
}

// -------------------------------------------------

bool TreephaserSSE::Solve(int begin_flow, int end_flow)
{
  sumNormMeasures();

  PathRec RESTRICT_PTR parent = sv_PathPtr[0];
  PathRec RESTRICT_PTR best = sv_PathPtr[MAX_PATHS];

  parent->flow = 0;
  parent->window_start = 0;
  parent->window_end = 1;
  parent->res = 0.0f;
  parent->metr = 0.0f;
  parent->flowMetr = 0.0f;
  parent->dotCnt = 0;
  parent->state[0] = 1.0f;
  parent->sequence_length = 0;
  parent->last_hp = 0;
  parent->pred[0] = 0.0f;

  int pathCnt = 1;
  float bestDist = 1e20; //float(endFlow);

  if(begin_flow > 0) {
    static const int char_to_nuc[8] = {-1, 0, -1, 1, 3, -1, -1, 2};
    for (int base = 0; base < best->sequence_length; ++base) {
      parent->sequence_length++;
      parent->sequence[base] = best->sequence[base];
      if (base and parent->sequence[base] != parent->sequence[base-1])
        parent->last_hp = 0;      
      parent->last_hp++;

       nextState(parent, char_to_nuc[best->sequence[base]&7], end_flow);
       for(int k = parent->window_start; k < parent->window_end; ++k) {
        if((k & 3) == 0) {
          sumVectFloatSSE(&parent->pred[k], &parent->state[k], parent->window_end-k);
          break;
        }
        parent->pred[k] += parent->state[k];
      }
      if (parent->flow >= begin_flow)
        break;
    }

    if(parent->window_end < begin_flow) {
      sv_PathPtr[MAX_PATHS] = parent;
      sv_PathPtr[0] = best;
      return true;
    }
    parent->res = sumOfSquaredDiffsFloatSSE(
      (float RESTRICT_PTR)rd_NormMeasure, (float RESTRICT_PTR)parent->pred, parent->window_start);
   }

  best->window_end = 0;
  best->sequence_length = 0;

  do {

    if(pathCnt > 3) {
      int m = sv_PathPtr[0]->flow;
      int i = 1;
      do {
        int n = sv_PathPtr[i]->flow;
        if(m < n)
          m = n;
      } while(++i < pathCnt);
      if((m -= MAX_PATH_DELAY) > 0) {
        do {
          if(sv_PathPtr[--i]->flow < m)
            swap(sv_PathPtr[i], sv_PathPtr[--pathCnt]);
        } while(i > 0);
      }
    }

    while(pathCnt > MAX_PATHS-4) {
      float m = sv_PathPtr[0]->flowMetr;
      int i = 1;
      int j = 0;
      do {
        float n = sv_PathPtr[i]->flowMetr;
        if(m < n) {
          m = n;
          j = i;
        }
      } while(++i < pathCnt);
      swap(sv_PathPtr[j], sv_PathPtr[--pathCnt]);
    }

    parent = sv_PathPtr[0];
    int parentPathIdx = 0;
    for(int i = 1; i < pathCnt; ++i)
      if(parent->metr > sv_PathPtr[i]->metr) {
        parent = sv_PathPtr[i];
        parentPathIdx = i;
      }
    if(parent->metr >= 1000.0f)
      break;
   int parent_flow = parent->flow;

    // compute child path flow states, predicted signal,negative and positive penalties
    advanceState4(parent, end_flow);

    int n = pathCnt;
    double bestpen = 25.0;
    for(int nuc = 0; nuc < 4; ++nuc) {
      PathRec RESTRICT_PTR child = sv_PathPtr[n];

      child->flow = ad_Idx[nuc];
      child->window_start = ad_Beg[nuc];
      child->window_end = ad_End[nuc];

      // seems to be a bug in third rule
      if(child->flow >= end_flow or parent->last_hp >= MAX_HPXLEN or parent->sequence_length >= 2*MAX_VALS-10)
        continue;

      // pointer in the ad_Buf buffer pointing at the running sum of positive residuals at start of parent window
      char RESTRICT_PTR pn = ad_Buf+nuc*4+(AD_NRES_OFS-16)-parent->window_start*16;

      // child path metric
      float metr = parent->res + *((float RESTRICT_PTR)(pn+child->window_start*16+(AD_PRES_OFS-AD_NRES_OFS)));

      // sum of squared residuals for positive residuals for flows < child->flow
      float penPar = *((float RESTRICT_PTR)(pn+child->flow*16+(AD_PRES_OFS-AD_NRES_OFS)));

      // sum of squared residuals for negative residuals for flows < child->window_end
      float penNeg = *((float RESTRICT_PTR)(pn+child->window_end*16));

      // sum of squared residuals left of child window start
      child->res = metr + *((float RESTRICT_PTR)(pn+child->window_start*16));
      metr += penNeg;

      // penPar corresponds to penalty1 in DPTreephaser.cpp
      penPar += penNeg;
      penNeg += penPar;

      // penalty = penalty1 + (kNegativeMultiplier = 2)*penNeg
      if(penNeg >= 20.0)
        continue;
 
      if(bestpen > penNeg)
        bestpen = penNeg;
      else if(penNeg-bestpen >= 0.2)
        continue;

      // child->path_metric > sum_of_squares_upper_bound
      if(metr > bestDist)
        continue;

      float newSignal = rd_NormMeasure[child->flow];
      // no check for this in swan in DPTreephaser.cpp
      if(child->flow < parent->window_end){
          if(!pm_model_enabled_)
            newSignal -= parent->pred[child->flow];
          else{
            newSignal -= (parent->calib_A[child->flow]*parent->pred[child->flow]+parent->calib_B[child->flow]);
          }

      }
      newSignal /= *((float RESTRICT_PTR)(pn+child->flow*16+(AD_STATE_OFS-AD_NRES_OFS+16)));
      child->dotCnt = 0;
      if(newSignal < 0.3f) {
        if(parent->dotCnt > 0)
          continue;
        child->dotCnt = 1;
      }
      // child path survives at this point
      child->metr = float(metr);
      child->flowMetr = float(penPar);
      child->penalty = float(penNeg);
      child->nuc = nuc;
      ++n;
    }

    // Computing squared distance between parent's predicted signal and normalized measurements
    float dist = parent->res+(rd_SqNormMeasureSum[parent->window_end]-rd_SqNormMeasureSum[end_flow]);
    for(int i = parent->window_start; i < parent->window_end; ++i) {
      if((i & 3) == 0) {
          if(!pm_model_enabled_)
              dist += sumOfSquaredDiffsFloatSSE((float RESTRICT_PTR)(&(rd_NormMeasure[i])), (float RESTRICT_PTR)(&(parent->pred[i])), parent->window_end-i);
          else
              dist += sumOfSquaredDiffsFloatSSE_recal((float RESTRICT_PTR)(&(rd_NormMeasure[i])),
                                                      (float RESTRICT_PTR)(&(parent->pred[i])), (float RESTRICT_PTR)(&(parent->calib_A[i])), (float RESTRICT_PTR)(&(parent->calib_B[i])), parent->window_end-i);
        break;
      }
      if(!pm_model_enabled_)
          dist += Sqr(rd_NormMeasure[i]-parent->pred[i]);
      else
          dist += Sqr(rd_NormMeasure[i]-parent->pred[i]*parent->calib_A[i]-parent->calib_B[i]);
    }
    // Finished computing squared distance

    int bestPathIdx = -1;

    // current best path is parent path
    if(bestDist > dist) {
      bestPathIdx = parentPathIdx;
      parentPathIdx = -1;
    }

    int childPathIdx = -1;
    while(pathCnt < n) {
      PathRec RESTRICT_PTR child = sv_PathPtr[pathCnt];
      // Rule that depends on finding the best nuc
      if(child->penalty-bestpen >= 0.2f) {
        sv_PathPtr[pathCnt] = sv_PathPtr[--n];
        sv_PathPtr[n] = child;
      } 
      else if((childPathIdx < 0) && (parentPathIdx >= 0)) {
        sv_PathPtr[pathCnt] = sv_PathPtr[--n];
        sv_PathPtr[n] = child;
        childPathIdx = n;
      }
      // this is the child path to be kept 
      else {
        child->flowMetr = (child->metr + 0.5f*child->flowMetr) / child->flow; // ??
        char RESTRICT_PTR p = ad_Buf+child->nuc*4+AD_STATE_OFS;
        for(int i = parent->window_start, j = 0, e = child->window_end; i < e; ++i, j += 16) {
          child->state[i] = *((float*)(p+j));
          child->pred[i] = *((float*)(p+j+(AD_PRED_OFS-AD_STATE_OFS)));
        }
        copySSE(child->pred, parent->pred, parent->window_start << 2);

        copySSE(child->sequence, parent->sequence, parent->sequence_length);

        if(state_inphase_enabled){
            if(child->flow > 0){
              int cpSize = (parent->flow+1) << 2;
              memcpy(child->state_inphase, parent->state_inphase, cpSize);
            }
            //extending from parent->state_inphase[parent->flow] to fill the gap
            for(int tempInd = parent->flow; tempInd <= child->flow; tempInd++){
                child->state_inphase[tempInd] = child->state[child->flow];
            }
        }

        child->sequence_length = parent->sequence_length + 1;
        child->sequence[parent->sequence_length] = flow_order_[child->flow];
        if (parent->sequence_length and child->sequence[parent->sequence_length] != child->sequence[parent->sequence_length-1])
          child->last_hp = 0;
        else
          child->last_hp = parent->last_hp;
        child->last_hp++;

        //explicitly fill zeros between parent->flow and child->flow;
        if(pm_model_enabled_){
          if(child->flow > 0){
            int cpSize = (parent->flow+1) << 2;
            memcpy(child->calib_A, parent->calib_A, cpSize);
            memcpy(child->calib_B, parent->calib_B, cpSize);
          }
          for(int tempInd = parent->flow + 1; tempInd < child->flow; tempInd++){
            child->calib_A[tempInd] = 1;
            child->calib_B[tempInd] = 0;
          }
          child->calib_A[child->flow] = (*As_)[child->flow][flow_order_.int_at(child->flow)][child->last_hp];
          child->calib_B[child->flow] = (*Bs_)[child->flow][flow_order_.int_at(child->flow)][child->last_hp];
        }
        ++pathCnt;
      }
    }

    // In the event, there is no best path, one of the child is copied to the parent
    if(childPathIdx >= 0) {
      PathRec RESTRICT_PTR child = sv_PathPtr[childPathIdx];
      parent_flow = parent->flow; //MJ
      parent->flow = child->flow;
      parent->window_end = child->window_end;
      parent->res = child->res;
      parent->metr = child->metr;
      parent->flowMetr = (child->metr + 0.5f*child->flowMetr) / child->flow; // ??
      parent->dotCnt = child->dotCnt;
      char RESTRICT_PTR p = ad_Buf+child->nuc*4+AD_STATE_OFS;
      for(int i = parent->window_start, j = 0, e = child->window_end; i < e; ++i, j += 16) {
        parent->state[i] = *((float*)(p+j));
        parent->pred[i] = *((float*)(p+j+(AD_PRED_OFS-AD_STATE_OFS)));
      }

      parent->sequence[parent->sequence_length] = flow_order_[parent->flow];
      if (parent->sequence_length and parent->sequence[parent->sequence_length] != parent->sequence[parent->sequence_length-1])
        parent->last_hp = 0;
      parent->last_hp++;
      parent->sequence_length++;

      //update calib_A and calib_B
      if(pm_model_enabled_){
          for(int tempInd = parent_flow + 1; tempInd < parent->flow; tempInd++){
            parent->calib_A[tempInd] = 1;
            parent->calib_B[tempInd] = 0;
          }
          parent->calib_A[parent->flow] = (*As_)[parent->flow][flow_order_.int_at(parent->flow)][parent->last_hp];
          parent->calib_B[parent->flow] = (*Bs_)[parent->flow][flow_order_.int_at(parent->flow)][parent->last_hp];
      }

      if(state_inphase_enabled){
          for(int tempInd = parent_flow; tempInd <= parent->flow; tempInd++){
              parent->state_inphase[tempInd] = parent->state[parent->flow];
          }
      }

      parent->window_start = child->window_start;
      parentPathIdx = -1;
    }

    // updating parent as best path
    if(bestPathIdx >= 0) {
      bestDist = dist;
      sv_PathPtr[bestPathIdx] = sv_PathPtr[--pathCnt];
      sv_PathPtr[pathCnt] = sv_PathPtr[MAX_PATHS];
      sv_PathPtr[MAX_PATHS] = parent;
    } else if(parentPathIdx >= 0) {
      sv_PathPtr[parentPathIdx] = sv_PathPtr[--pathCnt];
      sv_PathPtr[pathCnt] = parent;
    }

  } while(pathCnt > 0);

  return false;
}


void TreephaserSSE::WindowedNormalize(BasecallerRead& read, int num_steps)
{
//  int num_flows = read.raw_measurements.size();
  float median_set[windowSize_];

  // Estimate and correct for additive offset

  float next_normalizer = 0;
  int estim_flow = 0;
  int apply_flow = 0;

  for (int step = 0; step <= num_steps; ++step) {

    int window_end = estim_flow + windowSize_;
    int window_middle = estim_flow + windowSize_ / 2;
    if (window_middle > num_flows_)
      break;

    float normalizer = next_normalizer;

    int median_set_size = 0;
    for (; estim_flow < window_end and estim_flow < num_flows_ and estim_flow < sv_PathPtr[MAX_PATHS]->window_end; ++estim_flow)
      if (sv_PathPtr[MAX_PATHS]->pred[estim_flow] < 0.3)
        median_set[median_set_size++] = read.raw_measurements[estim_flow] - sv_PathPtr[MAX_PATHS]->pred[estim_flow];

    if (median_set_size > 5) {
      std::nth_element(median_set, median_set + median_set_size/2, median_set + median_set_size);
      next_normalizer = median_set[median_set_size / 2];
      if (step == 0)
        normalizer = next_normalizer;
    }

    float delta = (next_normalizer - normalizer) / static_cast<float>(windowSize_);

    for (; apply_flow < window_middle and apply_flow < num_flows_; ++apply_flow) {
      rd_NormMeasure[apply_flow] = read.raw_measurements[apply_flow] - normalizer;
      read.additive_correction[apply_flow] = normalizer;
      normalizer += delta;
    }
  }

  for (; apply_flow < num_flows_; ++apply_flow) {
    rd_NormMeasure[apply_flow] = read.raw_measurements[apply_flow] - next_normalizer;
    read.additive_correction[apply_flow] = next_normalizer;
  }

  // Estimate and correct for multiplicative scaling

  next_normalizer = 1;
  estim_flow = 0;
  apply_flow = 0;

  for (int step = 0; step <= num_steps; ++step) {

    int window_end = estim_flow + windowSize_;
    int window_middle = estim_flow + windowSize_ / 2;
    if (window_middle > num_flows_)
      break;

    float normalizer = next_normalizer;

    int median_set_size = 0;
    for (; estim_flow < window_end and estim_flow < num_flows_ and estim_flow < sv_PathPtr[MAX_PATHS]->window_end; ++estim_flow)
      if (sv_PathPtr[MAX_PATHS]->pred[estim_flow] > 0.5 and rd_NormMeasure[estim_flow] > 0)
        median_set[median_set_size++] = rd_NormMeasure[estim_flow] / sv_PathPtr[MAX_PATHS]->pred[estim_flow];

    if (median_set_size > 5) {
      std::nth_element(median_set, median_set + median_set_size/2, median_set + median_set_size);
      next_normalizer = median_set[median_set_size / 2];
      if (step == 0)
        normalizer = next_normalizer;
    }

    float delta = (next_normalizer - normalizer) / static_cast<float>(windowSize_);

    for (; apply_flow < window_middle and apply_flow < num_flows_; ++apply_flow) {
      rd_NormMeasure[apply_flow] /= normalizer;
      read.multiplicative_correction[apply_flow] = normalizer;
      normalizer += delta;
    }
  }

  for (; apply_flow < num_flows_; ++apply_flow) {
    rd_NormMeasure[apply_flow] /= next_normalizer;
    read.multiplicative_correction[apply_flow] = next_normalizer;
  }
}


// ------------------------------------------------------------------------
// Compute quality metrics

void  TreephaserSSE::ComputeQVmetrics(BasecallerRead& read)
{
  static const char nuc_int_to_char[5] = "ACGT";

  read.state_inphase.assign(flow_order_.num_flows(), 1);
  read.state_total.assign(flow_order_.num_flows(), 1);

  if (read.sequence.empty())
    return;

  read.penalty_mismatch.assign(read.sequence.size(), 0);
  read.penalty_residual.assign(read.sequence.size(), 0);

  PathRec RESTRICT_PTR parent = sv_PathPtr[0];
  PathRec RESTRICT_PTR children[4] = {sv_PathPtr[1], sv_PathPtr[2], sv_PathPtr[3], sv_PathPtr[4]};
  parent->flow = 0;
  parent->window_start = 0;
  parent->window_end = 1;
  parent->res = 0.0f;
  parent->metr = 0.0f;
  parent->flowMetr = 0.0f;
  parent->dotCnt = 0;
  parent->state[0] = 1.0f;
  parent->sequence_length = 0;
  parent->last_hp = 0;
  parent->pred[0] = 0.0f;

  float recent_state_inphase = 1;
  float recent_state_total = 1;

  // main loop for base calling
  for (int solution_flow = 0, base = 0; solution_flow < flow_order_.num_flows(); ++solution_flow) {
    for (; base < (int)read.sequence.size() and read.sequence[base] == flow_order_[solution_flow]; ++base) {

      float penalty[4] = { 0, 0, 0, 0 };

      int called_nuc = 0;

      // compute child path flow states, predicted signal,negative and positive penalties
      advanceState4(parent, flow_order_.num_flows());

      for(int nuc = 0; nuc < 4; ++nuc) {
        PathRec RESTRICT_PTR child = children[nuc];

        child->flow = ad_Idx[nuc];
        child->window_start = ad_Beg[nuc];
        child->window_end = ad_End[nuc];

        // Apply easy termination rules
        if (child->flow >= flow_order_.num_flows() || parent->last_hp >= MAX_HPXLEN ) {
          penalty[nuc] = 25; // Mark for deletion
          continue;
        }

        if (nuc_int_to_char[nuc] == flow_order_[solution_flow])
          called_nuc = nuc;

        // pointer in the ad_Buf buffer pointing at the running sum of positive residuals at start of parent window
        char RESTRICT_PTR pn = ad_Buf+nuc*4+(AD_NRES_OFS-16)-parent->window_start*16;

        // sum of squared residuals for positive residuals for flows < child->flow
        float penPar = *((float RESTRICT_PTR)(pn+child->flow*16+(AD_PRES_OFS-AD_NRES_OFS)));

        // sum of squared residuals for negative residuals for flows < child->window_end
        float penNeg = *((float RESTRICT_PTR)(pn+child->window_end*16));

        penalty[nuc] = penPar + penNeg;
      }

      // find current incorporating base
      assert(children[called_nuc]->flow == solution_flow);

      PathRec RESTRICT_PTR childToKeep = children[called_nuc];
      //copy
      char RESTRICT_PTR p = ad_Buf+ called_nuc*4 + AD_STATE_OFS;

      recent_state_total = 0;
      for(int i = parent->window_start, j = 0, e = childToKeep->window_end; i < e; ++i, j += 16) {
        childToKeep->state[i] = *((float*)(p+j));
        childToKeep->pred[i] = *((float*)(p+j+(AD_PRED_OFS-AD_STATE_OFS)));
        recent_state_total += childToKeep->state[i];
      }
      //sse implementation with aligned memory; no gain as the number of elements to be summed up is small
//      recent_state_total = vecSumSSE(state_Buf, countStates);

      copySSE(childToKeep->pred, parent->pred, parent->window_start << 2);

      if (childToKeep->flow == parent->flow)
        childToKeep->last_hp = parent->last_hp + 1;
      else
        childToKeep->last_hp = 1;

      recent_state_inphase = childToKeep->state[solution_flow];

      // Get delta penalty to next best solution
      read.penalty_mismatch[base] = -1; // min delta penalty to earlier base hypothesis
      read.penalty_residual[base] = 0;

      if (solution_flow - parent->window_start > 0)
        read.penalty_residual[base] = penalty[called_nuc] / (solution_flow - parent->window_start);

      for (int nuc = 0; nuc < 4; ++nuc) {
        if (nuc == called_nuc)
            continue;
        float penalty_mismatch = penalty[called_nuc] - penalty[nuc];
        read.penalty_mismatch[base] = max(read.penalty_mismatch[base], penalty_mismatch);
      }

      // Called state is the starting point for next base
      PathRec RESTRICT_PTR swap = parent;
      parent = children[called_nuc];
      children[called_nuc] = swap;
    }

    read.state_inphase[solution_flow] = max(recent_state_inphase, 0.01f);
    read.state_total[solution_flow] = max(recent_state_total, 0.01f);
  }

  setZeroSSE(&read.prediction[0], num_flows_*sizeof(float));
  copySSE(&read.prediction[0], parent->pred, parent->window_end*sizeof(float));
}




