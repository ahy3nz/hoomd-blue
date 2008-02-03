/*
Highly Optimized Object-Oriented Molecular Dynamics (HOOMD) Open
Source Software License
Copyright (c) 2008 Ames Laboratory Iowa State University
All rights reserved.

Redistribution and use of HOOMD, in source and binary forms, with or
without modification, are permitted, provided that the following
conditions are met:

* Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names HOOMD's
contributors may be used to endorse or promote products derived from this
software without specific prior written permission.

Disclaimer

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND
CONTRIBUTORS ``AS IS''  AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS  BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.
*/

// $Id$
// $URL$

#include "gpu_forces.h"
#include "gpu_pdata.h"
#include "gpu_utils.h"

#ifdef WIN32
#include <cassert>
#else
#include <assert.h>
#endif

/*! \file ljforcesum_kernel.cu
	\brief Contains code for the LJ force sum kernel on the GPU
	\details Functions in this file are NOT to be called by anyone not knowing 
		exactly what they are doing. They are designed to be used solely by 
		LJForceComputeGPU.
*/

///////////////////////////////////////// LJ params

//! The last used id generated by gpu_alloc_ljparam_data()
static int lj_current_id = 0;
//! The id of the last gpu_ljparam_data selected by gpu_select_ljparam_data()
static int lj_last_selected_id = -1;

//! Storage of the lj params in constant memory on the GPU
__constant__ gpu_ljparam_data c_ljparams;

//! Allocate ljparams
/*! \post Memory is allocated for the parameters and all values are set to 0.0f;
	\return Pointer to the allocated structure
	\note NOT THREAD SAFE! Only one set of parameters should be allocated on the host, though
		so this shouldn't matter too much.
*/
gpu_ljparam_data *gpu_alloc_ljparam_data()
	{
	// allocate memory
	gpu_ljparam_data *data = (gpu_ljparam_data *)malloc(sizeof(gpu_ljparam_data));
	assert(data);

	// clear
	for (int i = 0; i < MAX_NTYPE_PAIRS; i++)
		{
		data->lj1[i] = 0.0f;
		data->lj2[i] = 0.0f;
		}

	data->id = lj_current_id++;
	return data;
	}

/*! \post Allocated memory is freed on the host
	\param ljparams Parameter set to free
*/
void gpu_free_ljparam_data(gpu_ljparam_data *ljparams)
	{
	assert(ljparams);
	free(ljparams);
	}

/*! \post The parameters are copied to the constant memory on the device. If this was the 
	last structure selected (determined by id), then don't copy unless \a force is specified
	\returns error code passed on from cudaMemcpyToSymbol
	
	\param ljparams Parameter set to make active on the device
	\param force Force an update of the constant memory on the gpu.
*/
cudaError_t gpu_select_ljparam_data(gpu_ljparam_data *ljparams, bool force)
	{
	assert(ljparams);
	cudaError_t retval = cudaSuccess;

	if (ljparams->id != lj_last_selected_id || force)
		{
		retval = cudaMemcpyToSymbol(c_ljparams, ljparams, sizeof(gpu_ljparam_data));
		lj_last_selected_id = ljparams->id;
		}
	return retval;
	}

//! Hardcode the warp size for the divergence avoiding code
#define WARP_SIZE 32

//! Kernel for calculating lj forces
/*! This kerenel is called to calculate the lennard-jones forces on all N particles

	\param d_forces Device memory array to write calculated forces to
	\param pdata Particle data on the GPU to calculate forces on
	\param nlist Neigbhor list data on the GPU to use to calculate the forces
	\param r_cutsq Precalculated r_cut*r_cut, where r_cut is the radius beyond which forces are
		set to 0
	\param box Box dimensions used to implement periodic boundary conditions
	
	Developer information:
	Each block will calculate the forces on a block of particles.
	Each thread will calculate the total force on one particle.
	The neighborlist is arranged in columns so that reads are fully coalesced when doing this.
	This version dynamically avoids divergent warps
	It is also assumed that the neighborlist is padded out to have a number of columsn a 
	multiple of the block size so that reads can be simply done in a big block
	\bug Performance is extremely slow due to an implemented workaround for the annoying timeout bug
	\todo Remove limitation on excessively padded neighbor list
	\todo Remove the warp divergence avoiding code (it is here as part of the bug workaround)
*/
extern "C" __global__ void calcLJForces_kernel(float4 *d_forces, gpu_pdata_arrays pdata, gpu_nlist_array nlist, float r_cutsq, gpu_boxsize box)
	{
	// start by identifying which particle we are to handle
	int pidx = blockIdx.x * blockDim.x + threadIdx.x;
	
	// load in the length of the list (each thread loads it individually, 
	// we are going to dynamically avoid divergent warps by finding the maximum
	// length in the warp and looping over that for all threads in the warp
	// those that are less than the max length will hopefully be able to do a predicated instruction
	// in the loop to avoid diverging
	int n_neigh = nlist.list[pidx];

	// read in the position of our particle. Sure, this COULD be done as a fully coalesced global mem read
	// but reading it from the texture gives a slightly better performance, possibly because it "warms up" the
	// texture cache for the next read
	float4 pos = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
	if (pidx < pdata.N)
		{
		#ifdef USE_CUDA_BUG_WORKAROUND
		pos = pdata.pos[pidx];
		#else
		pos = tex1Dfetch(pdata_pos_tex, pidx);
		#endif
		}

	// initialize the force to 0
	float fx = 0.0f;
	float fy = 0.0f;
	float fz = 0.0f;

	// precompute a bit to tell wheter or not this particle needs periodic boundary conditions
	int period = 0;
	
	float dx = min(abs(pos.x - box.Lx/2.0f), abs(pos.x + box.Lx/2.0f));
	if (dx*dx <= r_cutsq)
		period = 1;
	float dy = min(abs(pos.y - box.Ly/2.0f), abs(pos.y + box.Ly/2.0f));
	if (dy*dy <= r_cutsq)
		period = 1;
	float dz = min(abs(pos.z - box.Lz/2.0f), abs(pos.z + box.Lz/2.0f));
	if (dz*dz <= r_cutsq)
		period = 1;
	
	// loop over neighbors
	for (int neigh_idx = 0; neigh_idx < n_neigh/*sdata[start]*/; neigh_idx++)
		{
		int cur_neigh = nlist.list[nlist.pitch*(neigh_idx+1) + pidx];
	
		// get the neighbor's position
		float4 neigh_pos = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
		if (pidx < pdata.N)
			{
			#ifdef USE_CUDA_BUG_WORKAROUND
			neigh_pos = pdata.pos[cur_neigh];
			#else
			neigh_pos = tex1Dfetch(pdata_pos_tex, cur_neigh);
			#endif
			}

		float nx = neigh_pos.x;
		float ny = neigh_pos.y;
		float nz = neigh_pos.z;
		int neigh_typ = __float_as_int(neigh_pos.w);
	
		// calculate dr (with periodic boundary conditions)
		float dx = pos.x - nx;
		float dy = pos.y - ny;
		float dz = pos.z - nz;
		
		if (period)
			{
			dx -= box.Lx * rintf(dx * box.Lxinv);
			dy -= box.Ly * rintf(dy * box.Lyinv);
			dz -= box.Lz * rintf(dz * box.Lzinv);
			}
			
		float rsq = dx*dx + dy*dy + dz*dz;
		
		float r2inv;
		if (rsq >= r_cutsq/* || neigh_idx >= sdata[blockDim.x + threadIdx.x]*/)
			r2inv = 0.0f;
		else
			r2inv = 1.0f / rsq;

		int ptype = __float_as_int(pos.w);
		int typ_pair = neigh_typ * MAX_NTYPES + ptype;

		// note that the code for calculating the force was borrowed from lammps
		float r6inv = r2inv*r2inv*r2inv;
		// the lj1 and lj2 params are encoded into the x and y components of the params array
		float lj1 = c_ljparams.lj1[typ_pair];
		float lj2 = c_ljparams.lj2[typ_pair];
		float fforce = r2inv * r6inv * (lj1  * r6inv - lj2);
		
		// add up the forces
		fx += dx * fforce;
		fy += dy * fforce;
		fz += dz * fforce;
		}
	
	// now that the force calculation is complete, write out the result if we are a valid particle
	if (pidx < pdata.N)
		{
		float4 force;
		force.x = fx;
		force.y = fy;
		force.z = fz;
		force.w = 0.0f;
		d_forces[pidx] = force;
		}
	
	}


/*! \param d_forces Device memory to write forces to
	\param pdata Particle data on the GPU to perform the calculation on
	\param box Box dimensions (in GPU format) to use for periodic boundary conditions
	\param nlist Neighbor list stored on the gpu
	\param r_cutsq Precomputed r_cut*r_cut, where r_cut is the radius beyond which the 
		force is set to 0
	\param M Block size to execute
	
	\returns Any error code resulting from the kernel launch
	\note Always returns cudaSuccess in release builds to avoid the cudaThreadSynchronize()
*/
cudaError_t gpu_ljforce_sum(float4 *d_forces, gpu_pdata_arrays *pdata, gpu_boxsize *box, gpu_nlist_data *nlist, float r_cutsq, int M)
	{
	assert(pdata);
	assert(nlist);
	// check that M is valid
	assert(M != 0);
	assert((M & 31) == 0);
	assert(M <= 512);

    // setup the grid to run the kernel
    dim3 grid( (int)ceil((double)pdata->N/ (double)M), 1, 1);
    dim3 threads(M, 1, 1);

    // run the kernel
    calcLJForces_kernel<<< grid, threads >>>(d_forces, *pdata, nlist->d_array, r_cutsq, *box);

	#ifdef NDEBUG
	return cudaSuccess;
	#else
	cudaThreadSynchronize();
	return cudaGetLastError();
	#endif
	}

// vim:syntax=cpp
