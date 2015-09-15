#pragma once

#ifndef _SOLVER_EQUATIONS_
#define _SOLVER_EQUATIONS_

#define THREADS_PER_BLOCK_JT 128

#include <cutil_inline.h>
#include <cutil_math.h>

#include "../SiftGPU/cuda_SimpleMatrixUtil.h"

#include "SolverBundlingUtil.h"
#include "SolverBundlingState.h"
#include "SolverBundlingParameters.h"

#include "ICPUtil.h"

// not squared!
__inline__ __device__ float evalResidualDeviceFloat3(unsigned int corrIdx, unsigned int componentIdx, SolverInput& input, SolverState& state, SolverParameters& parameters)
{
	float3 r = make_float3(0.0f, 0.0f, 0.0f);

	const EntryJ& corr = input.d_correspondences[corrIdx];
	if (corr.isValid()) {
		float3x3 TI = evalRMat(state.d_xRot[corr.imgIdx_i]);
		float3x3 TJ = evalRMat(state.d_xRot[corr.imgIdx_j]);

		r = fabs((TI*corr.pos_i + state.d_xTrans[corr.imgIdx_i]) - (TJ*corr.pos_j + state.d_xTrans[corr.imgIdx_j]));
		if (componentIdx == 0) return r.x;
		if (componentIdx == 1) return r.y;
		return r.z; //if (componentIdx == 2) 
	}
	return 0.0f;
}

__inline__ __device__ float evalFDevice(unsigned int corrIdx, SolverInput& input, SolverState& state, SolverParameters& parameters)
{
	float3 r = make_float3(0.0f, 0.0f, 0.0f);

	const EntryJ& corr = input.d_correspondences[corrIdx];
	if (corr.isValid()) {
		float3x3 TI = evalRMat(state.d_xRot[corr.imgIdx_i]);
		float3x3 TJ = evalRMat(state.d_xRot[corr.imgIdx_j]);

		r = (TI*corr.pos_i + state.d_xTrans[corr.imgIdx_i]) - (TJ*corr.pos_j + state.d_xTrans[corr.imgIdx_j]);

		float res = dot(r, r);
		return res;
	}
	return 0.0f;
}

////////////////////////////////////////
// applyJT : this function is called per variable and evaluates each residual influencing that variable (i.e., each energy term per variable)
////////////////////////////////////////

__inline__ __device__ void evalMinusJTFDevice(unsigned int variableIdx, SolverInput& input, SolverState& state, SolverParameters& parameters, float3& resRot, float3& resTrans)
{
	float3 rRot   = make_float3(0.0f, 0.0f, 0.0f);
	float3 rTrans = make_float3(0.0f, 0.0f, 0.0f);

	float3 pRot   = make_float3(0.0f, 0.0f, 0.0f);
	float3 pTrans = make_float3(0.0f, 0.0f, 0.0f);

	// Reset linearized update vector
	state.d_deltaRot[variableIdx]   = make_float3(0.0f, 0.0f, 0.0f);
	state.d_deltaTrans[variableIdx] = make_float3(0.0f, 0.0f, 0.0f);

	// Compute -JTF here
	int N = input.d_numEntriesPerRow[variableIdx];

	const float3&  oldAngles0 = state.d_xRot[variableIdx]; // get angles
	const float3x3 R_dAlpha = evalR_dAlpha(oldAngles0);
	const float3x3 R_dBeta = evalR_dBeta(oldAngles0);
	const float3x3 R_dGamma = evalR_dGamma(oldAngles0);

	for (int i = 0; i < N; i++)
	{
		int corrIdx = input.d_variablesToCorrespondences[variableIdx*input.maxCorrPerImage + i];
		const EntryJ& corr = input.d_correspondences[corrIdx];
		if (corr.isValid()) {
			float3 variableP = corr.pos_i;
			float  variableSign = 1;
			if (variableIdx != corr.imgIdx_i)
			{
				variableP = corr.pos_j;
				variableSign = -1;
			}

			const float3x3 TI = evalRMat(state.d_xRot[corr.imgIdx_i]);
			const float3x3 TJ = evalRMat(state.d_xRot[corr.imgIdx_j]);
			const float3 r = (TI*corr.pos_i + state.d_xTrans[corr.imgIdx_i]) - (TJ*corr.pos_j + state.d_xTrans[corr.imgIdx_j]);

			//float3 wp = TI*corr.p0+state.d_xTrans[corr.idx0];
			//float3 wq = TJ*corr.p1+state.d_xTrans[corr.idx1];

			rRot += variableSign*make_float3(dot(R_dAlpha*variableP, r), dot(R_dBeta*variableP, r), dot(R_dGamma*variableP, r));
			rTrans += variableSign*r;

			pRot += make_float3(dot(R_dAlpha*variableP, R_dAlpha*variableP), dot(R_dBeta*variableP, R_dBeta*variableP), dot(R_dGamma*variableP, R_dGamma*variableP));
			pTrans += make_float3(1.0f, 1.0f, 1.0f);
		}
	}

	resRot	 = -rRot;
	resTrans = -rTrans;

	// Preconditioner depends on last solution P(input.d_x)
	if (pRot.x > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].x   = 1.0f/pRot.x;
	else					      state.d_precondionerRot[variableIdx].x   = 1.0f;

	if (pRot.y > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].y   = 1.0f/pRot.y;
	else					      state.d_precondionerRot[variableIdx].y   = 1.0f;

	if (pRot.z > FLOAT_EPSILON)   state.d_precondionerRot[variableIdx].z   = 1.0f/pRot.z;
	else						  state.d_precondionerRot[variableIdx].z   = 1.0f;

	if (pTrans.x > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].x = 1.0f / pTrans.x;
	else					      state.d_precondionerTrans[variableIdx].x = 1.0f;

	if (pTrans.y > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].y = 1.0f / pTrans.y;
	else					      state.d_precondionerTrans[variableIdx].y = 1.0f;

	if (pTrans.z > FLOAT_EPSILON) state.d_precondionerTrans[variableIdx].z = 1.0f / pTrans.z;
	else					      state.d_precondionerTrans[variableIdx].z = 1.0f;
}

////////////////////////////////////////
// applyJT : this function is called per variable and evaluates each residual influencing that variable (i.e., each energy term per variable)
////////////////////////////////////////

//__inline__ __device__ void applyJTDevice(unsigned int variableIdx, SolverInput& input, SolverState& state, const SolverParameters& parameters, float3& outRot, float3& outTrans, unsigned int lane)
//{
//	// Compute J^T*d_Jp here
//	outRot	 = make_float3(0.0f, 0.0f, 0.0f);
//	outTrans = make_float3(0.0f, 0.0f, 0.0f);
//
//	const float3&  oldAngles0 = state.d_xRot[variableIdx]; // get angles
//	const float3x3 R_dAlpha = evalR_dAlpha(oldAngles0);
//	const float3x3 R_dBeta  = evalR_dBeta (oldAngles0);
//	const float3x3 R_dGamma = evalR_dGamma(oldAngles0);
//
//	int N = input.d_numEntriesPerRow[variableIdx];
//
//	for (int i = lane; i < N; i+=WARP_SIZE)
//	{
//		int corrIdx = input.d_variablesToCorrespondences[variableIdx*input.maxCorrPerImage + i];
//		const Correspondence& corr = input.d_correspondences[corrIdx];
//
//		float3 variableP = corr.p0;
//		float  variableSign = 1;
//		if (variableIdx != corr.idx0)
//		{
//			variableP	 = corr.p1;
//			variableSign = -1;
//		}
//
//		outRot   += variableSign * make_float3(dot(R_dAlpha*variableP, state.d_Jp[corrIdx]), dot(R_dBeta*variableP, state.d_Jp[corrIdx]), dot(R_dGamma*variableP, state.d_Jp[corrIdx]));
//		outTrans += variableSign * state.d_Jp[corrIdx];
//	}
//
//	outRot.x   = warpReduce(outRot.x);	 outRot.y   = warpReduce(outRot.y);	  outRot.z   = warpReduce(outRot.z);
//	outTrans.x = warpReduce(outTrans.x); outTrans.y = warpReduce(outTrans.y); outTrans.z = warpReduce(outTrans.z);
//}

__inline__ __device__ void applyJTDevice(unsigned int variableIdx, SolverInput& input, SolverState& state, const SolverParameters& parameters, float3& outRot, float3& outTrans, unsigned int threadIdx, unsigned int lane)
{
	// Compute J^T*d_Jp here
	outRot	 = make_float3(0.0f, 0.0f, 0.0f);
	outTrans = make_float3(0.0f, 0.0f, 0.0f);

	const float3&  oldAngles0 = state.d_xRot[variableIdx]; // get angles
	const float3x3 R_dAlpha = evalR_dAlpha(oldAngles0);
	const float3x3 R_dBeta  = evalR_dBeta (oldAngles0);
	const float3x3 R_dGamma = evalR_dGamma(oldAngles0);

	int N = input.d_numEntriesPerRow[variableIdx];

	for (int i = threadIdx; i < N; i += THREADS_PER_BLOCK_JT)
	{
		int corrIdx = input.d_variablesToCorrespondences[variableIdx*input.maxCorrPerImage + i];
		const EntryJ& corr = input.d_correspondences[corrIdx];
		if (corr.isValid()) {
			float3 variableP = corr.pos_i;
			float  variableSign = 1;
			if (variableIdx != corr.imgIdx_i)
			{
				variableP = corr.pos_j;
				variableSign = -1;
			}

			outRot += variableSign * make_float3(dot(R_dAlpha*variableP, state.d_Jp[corrIdx]), dot(R_dBeta*variableP, state.d_Jp[corrIdx]), dot(R_dGamma*variableP, state.d_Jp[corrIdx]));
			outTrans += variableSign * state.d_Jp[corrIdx];
		}
	}

	outRot.x   = warpReduce(outRot.x);	 outRot.y   = warpReduce(outRot.y);	  outRot.z   = warpReduce(outRot.z);
	outTrans.x = warpReduce(outTrans.x); outTrans.y = warpReduce(outTrans.y); outTrans.z = warpReduce(outTrans.z);
}

__inline__ __device__ float3 applyJDevice(unsigned int corrIdx, SolverInput& input, SolverState& state, const SolverParameters& parameters)
{
	// Compute Jp here
	float3 b = make_float3(0.0f, 0.0f, 0.0f);
	const EntryJ& corr = input.d_correspondences[corrIdx];
	
	if (corr.isValid()) {
		if (corr.imgIdx_i > 0)	// get transform 0
		{
			const float3& oldAngles0 = state.d_xRot[corr.imgIdx_i]; // get angles
			const float3  dAlpha0 = evalR_dAlpha(oldAngles0)*corr.pos_i;
			const float3  dBeta0 = evalR_dBeta(oldAngles0)*corr.pos_i;
			const float3  dGamma0 = evalR_dGamma(oldAngles0)*corr.pos_i;
			const float3  pp0 = state.d_pRot[corr.imgIdx_i];
			b += dAlpha0*pp0.x + dBeta0*pp0.y + dGamma0*pp0.z + state.d_pTrans[corr.imgIdx_i];
		}

		if (corr.imgIdx_j > 0)	// get transform 1
		{
			const float3& oldAngles1 = state.d_xRot[corr.imgIdx_j]; // get angles
			const float3  dAlpha1 = evalR_dAlpha(oldAngles1)*corr.pos_j;
			const float3  dBeta1 = evalR_dBeta(oldAngles1)*corr.pos_j;
			const float3  dGamma1 = evalR_dGamma(oldAngles1)*corr.pos_j;
			const float3  pp1 = state.d_pRot[corr.imgIdx_j];
			b -= dAlpha1*pp1.x + dBeta1*pp1.y + dGamma1*pp1.z + state.d_pTrans[corr.imgIdx_j];
		}
	}
	return b;
}

#endif