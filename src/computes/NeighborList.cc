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

// windows is gay, and needs this to define pi
#define _USE_MATH_DEFINES
#include <math.h>

#ifdef USE_PYTHON
#include <boost/python.hpp>
using namespace boost::python;
#endif

#include "NeighborList.h"

#include <sstream>
#include <fstream>

#ifdef USE_SSE
#ifdef __SSE__
#include <xmmintrin.h>
#endif

#ifdef __SSE2__
#include <emmintrin.h>
#endif
#endif

#include <iostream>
#include <stdexcept>
using namespace std;

/*! \file NeighborList.cc
	\brief Contains code for the NeighborList class
*/

/*! \param pdata Particle data the neighborlist is to compute neighbors for
	\param r_cut Cuttoff radius under which particles are considered neighbors
	\param r_buff Buffere radius around \a r_cut in which neighbors will be included
	
	\post NeighborList is initialized and the list memory has been allocated,
		but the list will not be computed until compute is called.
	\post The storage mode defaults to half
*/
NeighborList::NeighborList(boost::shared_ptr<ParticleData> pdata, Scalar r_cut, Scalar r_buff) 
	: Compute(pdata), m_r_cut(r_cut), m_r_buff(r_buff), m_storage_mode(half), m_force_update(true), m_updates(0), m_forced_updates(0)
	{
	// check for two sensless errors the user could make
	if (m_r_cut < 0.0)
		throw runtime_error("Requested cuttoff radius for neighborlist less than zero");
	
	if (m_r_buff < 0.0)
		throw runtime_error("Cuttoff radius for neighborlist less than zero");
		
	// allocate the list memory
	m_list.resize(pdata->getN());
	m_exclusions.resize(pdata->getN());

	// allocate memory for storing the last particle positions
	m_last_x = new Scalar[pdata->getN()];
	m_last_y = new Scalar[pdata->getN()];
	m_last_z = new Scalar[pdata->getN()];

	assert(m_last_x);
	assert(m_last_y);
	assert(m_last_z);

	// zero data
	memset((void*)m_last_x, 0, sizeof(Scalar)*pdata->getN());
	memset((void*)m_last_y, 0, sizeof(Scalar)*pdata->getN());
	memset((void*)m_last_z, 0, sizeof(Scalar)*pdata->getN());
	
	m_last_updated_tstep = 0;
	m_every = 0;
	
	#ifdef USE_CUDA
	// initialize the GPU and CPU mirror structures
	// there really should be a better way to determine the initial height, but we will just
	// choose a given value for now (choose it initially small to test the auto-expansion 
	// code
	gpu_alloc_nlist_data(&m_gpu_nlist, pdata->getN(), 256);
	m_data_location = cpugpu;
	hostToDeviceCopy();
	#endif
	}

NeighborList::~NeighborList()
	{
	delete[] m_last_x;
	delete[] m_last_y;
	delete[] m_last_z;
	
	#ifdef USE_CUDA
	gpu_free_nlist_data(&m_gpu_nlist);
	#endif
	}

/*! \param every Number of time steps to wait before beignning to check if particles have moved a sufficient distance
	to require a neighbor list upate.
*/
void NeighborList::setEvery(unsigned int every)
	{
	m_every = every;
	}
		 
/*! Updates the neighborlist if it has not yet been updated this times step
 	\param timestep Current time step of the simulation
*/
void NeighborList::compute(unsigned int timestep)
	{
	checkForceUpdate();
	// skip if we shouldn't compute this step
	if (!shouldCompute(timestep) && !m_force_update)
		return;

	if (m_prof)
		m_prof->push("Nlist^2");
	
	if (needsUpdating(timestep))
		{
		computeSimple();
		
		#ifdef USE_CUDA
		// after computing, the device now resides on the CPU
		m_data_location = cpu;
		#endif
		}
	
	if (m_prof)
		m_prof->pop();
	}
		 
/*! \param r_cut New cuttoff radius to set
	\param r_buff New buffer radius to set
	\note Changing the cuttoff radius does NOT immeadiately update the neighborlist.
 			The new cuttoff will take effect when compute is called for the next timestep.
*/
void NeighborList::setRCut(Scalar r_cut, Scalar r_buff)
	{
	m_r_cut = r_cut;
	m_r_buff = r_buff;
	
	// check for two sensless errors the user could make
	if (m_r_cut < 0.0)
		throw runtime_error("Requested cuttoff radius for neighborlist less than zero");
	
	if (m_r_buff < 0.0)
		throw runtime_error("Cuttoff radius for neighborlist less than zero");
				
	forceUpdate();
	}
		
/*! \return Reference to the neighbor list table
 	If the neighbor list was last updated on the GPU, it is copied to the CPU first.
	This copy operation is only intended for debugging and status information purposes.
	It is not optimized in any way, and is quite slow.
*/
const std::vector< std::vector<unsigned int> >& NeighborList::getList()
	{
	#ifdef USE_CUDA
	
	// this is the complicated graphics card version, need to do some work
	// switch based on the current location of the data
	switch (m_data_location)
		{
		case cpu:
			// if the data is solely on the cpu, life is easy, return the data arrays
			// and stay in the same state
			return m_list;
			break;
		case cpugpu:
			// if the data is up to date on both the cpu and gpu, life is easy, return
			// the data arrays and stay in the same state
			return m_list;
			break;
		case gpu:
			// if the data resides on the gpu, it needs to be copied back to the cpu
			// this changes to the cpugpu state since the data is now fully up to date on 
			// both
			deviceToHostCopy();
			m_data_location = cpugpu;
			return m_list;
			break;
		default:
			// anything other than the above is an undefined state!
			assert(false);
			return m_list;	
			break;			
		}

	#else
	
	return m_list;
	#endif
	}

/*! \returns an estimate of the number of neighbors per particle
	This mean-field estimate may be very bad dending on how clustered particles are.
	Derived classes can override this method to provide better estimates.

	\note Under NO circumstances should calling this method produce any 
	appreciable amount of overhead. This is mainly a warning to
	derived classes.
*/
Scalar NeighborList::estimateNNeigh()
	{
	// calculate a number density of particles
	BoxDim box = m_pdata->getBox();
	Scalar vol = (box.xhi - box.xlo)*(box.yhi - box.ylo)*(box.zhi - box.zlo);
	Scalar n_dens = Scalar(m_pdata->getN()) / vol;
	
	// calculate the average number of neighbors by multiplying by the volume
	// within the cutoff
	Scalar r_max = m_r_cut + m_r_buff;
	Scalar vol_cut = Scalar(4.0/3.0 * M_PI) * r_max * r_max * r_max;
	return n_dens * vol_cut;
	}

#ifdef USE_CUDA
/*! \returns Neighbor list data structure stored on the GPU.
	If the neighbor list was last updated on the CPU, calling this routine will result
	in a very time consuming copy to the device. It is meant only as a debugging/testing
	path and not for production simulations.
*/
gpu_nlist_data NeighborList::getListGPU()
	{
	// this is the complicated graphics card version, need to do some work
	// switch based on the current location of the data
	switch (m_data_location)
		{
		case cpu:
			// if the data is on the cpu, we need to copy it over to the gpu
			hostToDeviceCopy();
			// now we are in the cpugpu state
			m_data_location = cpugpu;
			return m_gpu_nlist;
			break;
		case cpugpu:
			// if the data is up to date on both the cpu and gpu, life is easy
			// state remains the same
			return m_gpu_nlist;
			break;
		case gpu:
			// if the data resides on the gpu, life is easy, just make sure that 
			// state remains the same
			return m_gpu_nlist;
			break;
		default:
			// anything other than the above is an undefined state!
			assert(false);
			return m_gpu_nlist;	
			break;			
		}
	}

/*! \post The entire neighbor list is copied from the CPU to the GPU.
	The copy is not optimized in any fashion and will be quite slow.
*/
void NeighborList::hostToDeviceCopy()
	{
	if (m_prof)
		m_prof->push("NLIST C2G");
		
	// start by determining if we need to make the device list larger or not
	// find the maximum neighborlist height
	unsigned int max_h = 0;
	for (unsigned int i = 0; i < m_pdata->getN(); i++)
		{
		if (m_list[i].size() > max_h)
			max_h = m_list[i].size();
		}
			
	// if the largest nlist is bigger than the capacity of the device nlist,
	// make it 10% bigger (note that the capacity of the device list is height-1 since
	// the number of neighbors is stored in the first row)
	if (max_h > m_gpu_nlist.h_array.height - 1)
		{
		gpu_free_nlist_data(&m_gpu_nlist);
		gpu_alloc_nlist_data(&m_gpu_nlist, m_pdata->getN(), (unsigned int)(float(max_h)*1.1));
		}
	
	// now we are good to copy the data over
	// start by zeroing the list
	memset(m_gpu_nlist.h_array.list, 0, sizeof(unsigned int) * m_gpu_nlist.h_array.pitch * m_gpu_nlist.h_array.height);
	
	for (unsigned int i = 0; i < m_pdata->getN(); i++)
		{
		// fill out the first row with the length of each list
		m_gpu_nlist.h_array.list[i] = m_list[i].size();
		
		// now fill out the data
		for (unsigned int j = 0; j < m_list[i].size(); j++)
			m_gpu_nlist.h_array.list[(j+1)*m_gpu_nlist.h_array.pitch + i] = m_list[i][j];
		}
	
	// now that the host array is filled out, copy it to the card
	gpu_copy_nlist_data_htod(&m_gpu_nlist);
	
	if (m_prof)
		m_prof->pop();
	}

/*! \post The entire neighbor list is copied from the GPU to the CPU.
	The copy is not optimized in any fashion and will be quite slow.
*/			
void NeighborList::deviceToHostCopy()
	{
	if (m_prof)
		m_prof->push("NLIST G2C");
		
	// copy data back from the card
	gpu_copy_nlist_data_dtoh(&m_gpu_nlist);
	
	// clear out version of the list
	for (unsigned int i = 0; i < m_pdata->getN(); i++)
		{
		m_list[i].clear();
		
		// now loop over all elements in the array
		unsigned int size = m_gpu_nlist.h_array.list[i];
		for (unsigned int j = 0; j < size; j++)
			m_list[i].push_back(m_gpu_nlist.h_array.list[(j+1)*m_gpu_nlist.h_array.pitch + i]);
		}
	
	if (m_prof)
		m_prof->pop();
	}

/*! \post The exclusion list is converted from tags to indicies and then copied up to the
	GPU.
*/
void NeighborList::updateExclusionData()
	{
	ParticleDataArraysConst arrays = m_pdata->acquireReadOnly();
	
	// setup each of the exclusions
	for (unsigned int tag_i = 0; tag_i < m_pdata->getN(); tag_i++)
		{
		unsigned int i = arrays.rtag[tag_i];
		if (m_exclusions[tag_i].e1 == EXCLUDE_EMPTY)
			m_gpu_nlist.h_array.exclusions[i].x = EXCLUDE_EMPTY;
		else
			m_gpu_nlist.h_array.exclusions[i].x = arrays.rtag[m_exclusions[tag_i].e1];
			
		if (m_exclusions[tag_i].e2 == EXCLUDE_EMPTY)
			m_gpu_nlist.h_array.exclusions[i].y = EXCLUDE_EMPTY;
		else
			m_gpu_nlist.h_array.exclusions[i].y = arrays.rtag[m_exclusions[tag_i].e2];

		if (m_exclusions[tag_i].e3 == EXCLUDE_EMPTY)
			m_gpu_nlist.h_array.exclusions[i].z = EXCLUDE_EMPTY;
		else
			m_gpu_nlist.h_array.exclusions[i].z = arrays.rtag[m_exclusions[tag_i].e3];

		if (m_exclusions[tag_i].e4 == EXCLUDE_EMPTY)
			m_gpu_nlist.h_array.exclusions[i].w = EXCLUDE_EMPTY;
		else
			m_gpu_nlist.h_array.exclusions[i].w = arrays.rtag[m_exclusions[tag_i].e4];
		}
	gpu_copy_exclude_data_htod(&m_gpu_nlist);
	
	m_pdata->release();
	}		

#endif
	
/*! \param mode Storage mode to set
	- half only stores neighbors where i < j
	- full stores all neighbors
	
	The neighborlist is not immediately updated to reflect this change. It will take effect
	when compute is called for the next timestep.
*/
void NeighborList::setStorageMode(storageMode mode)
	{
	m_storage_mode = mode;
	forceUpdate();
	}

/*! \param tag1 TAG (not index) of the first particle in the pair
	\param tag2 TAG (not index) of the second particle in the pair
	\post The pair \a tag1, \a tag2 will not appear in the neighborlist
	\note This only takes effect on the next call to compute() that updates the list
	\note Only 4 particles can be excluded from a single particle's neighbor list
	\note It is the caller's responsibility to not add duplicate entries
*/
void NeighborList::addExclusion(unsigned int tag1, unsigned int tag2)
	{
	if (tag1 >= m_pdata->getN() || tag2 >= m_pdata->getN())
		{
		cerr << "Particle tag out of bounds when attempting to add neighborlist exclusion: " << tag1 << "," << tag2 << endl;
		throw runtime_error("Invalid tag specification in NeighborList::addExclusion");
		}
		
	// add tag2 to tag1's exculsion list
	if (m_exclusions[tag1].e1 == EXCLUDE_EMPTY)
		m_exclusions[tag1].e1 = tag2;
	else if (m_exclusions[tag1].e2 == EXCLUDE_EMPTY)
		m_exclusions[tag1].e2 = tag2;
	else if (m_exclusions[tag1].e3 == EXCLUDE_EMPTY)
		m_exclusions[tag1].e3 = tag2;
	else if (m_exclusions[tag1].e4 == EXCLUDE_EMPTY)
		m_exclusions[tag1].e4 = tag2;
	else
		{
		// error: exclusion list full
		cerr << "Exclusion list full for particle with tag: " << tag1 << endl;
		throw runtime_error("Tag list full in NeighborList::addExclusion");
		}

	// add tag1 to tag2's exclusion list
	if (m_exclusions[tag2].e1 == EXCLUDE_EMPTY)
		m_exclusions[tag2].e1 = tag1;
	else if (m_exclusions[tag2].e2 == EXCLUDE_EMPTY)
		m_exclusions[tag2].e2 = tag1;
	else if (m_exclusions[tag2].e3 == EXCLUDE_EMPTY)
		m_exclusions[tag2].e3 = tag1;
	else if (m_exclusions[tag2].e4 == EXCLUDE_EMPTY)
		m_exclusions[tag2].e4 = tag1;
	else
		{
		// error: exclusion list full
		cerr << "Exclusion list full for particle with tag: " << tag2 << endl;
		throw runtime_error("Tag list full in NeighborList::addExclusion");
		}
	forceUpdate();
	}

/*! \returns true If any of the particles have been moved more than 1/2 of the buffer distance since the last call
		to this method that returned true.
	\returns false If none of the particles has been moved more than 1/2 of the buffer distance since the last call to this
		method that returned true.
	\note This is designed to be called if (needsUpdating()) then update every step. It internally handles the copy
		of the particle data into the last arrays so the caller doesn't need to.
	\param timestep Current time step in the simulation
*/
bool NeighborList::needsUpdating(unsigned int timestep)
	{
	// perform an early check: specifiying m_r_buff = 0.0 will result in the neighbor list being
	// updated every single time.
	if (m_r_buff < 1e-6)
		return true;
	if (timestep < (m_last_updated_tstep + m_every) && !m_force_update)
		return false;
			
	// scan through the particle data arrays and calculate distances
	if (m_prof)
		m_prof->push("Dist check");	

	const ParticleDataArraysConst& arrays = m_pdata->acquireReadOnly(); 
	// sanity check
	assert(arrays.x != NULL && arrays.y != NULL && arrays.z != NULL);

	bool result = false;

	// if the update has been forced, the result defaults to true
	if (m_force_update)
		{
		result = true;
		m_force_update = false;
		m_forced_updates += m_pdata->getN();
		m_last_updated_tstep = timestep;
		}
	else
		{
		// get a local copy of the simulation box too
		const BoxDim& box = m_pdata->getBox();
		// sanity check
		assert(box.xhi > box.xlo && box.yhi > box.ylo && box.zhi > box.zlo);
	
		// precalculate box lenghts
		Scalar Lx = box.xhi - box.xlo;
		Scalar Ly = box.yhi - box.ylo;
		Scalar Lz = box.zhi - box.zlo;

		// actually scan the array looking for values over 1/2 the buffer distance
		Scalar maxsq = (m_r_buff/Scalar(2.0))*(m_r_buff/Scalar(2.0));
		for (unsigned int i = 0; i < arrays.nparticles; i++)
			{
			Scalar dx = arrays.x[i] - m_last_x[i];
			Scalar dy = arrays.y[i] - m_last_y[i];
			Scalar dz = arrays.z[i] - m_last_z[i];
			
			// if the vector crosses the box, pull it back
			if (dx >= box.xhi)
				dx -= Lx;
			else
			if (dx < box.xlo)
				dx += Lx;
			
			if (dy >= box.yhi)
				dy -= Ly;
			else
			if (dy < box.ylo)
				dy += Ly;
						
			if (dz >= box.zhi)
				dz -= Lz;
			else
			if (dz < box.zlo)
				dz += Lz;

			if (dx*dx + dy*dy + dz*dz >= maxsq)
				{
				result = true;
				m_updates += m_pdata->getN();
				break;
				}
			}
		}

	// if we are updating, update the last position arrays
	if (result)
		{
		memcpy((void *)m_last_x, arrays.x, sizeof(Scalar)*arrays.nparticles);
		memcpy((void *)m_last_y, arrays.y, sizeof(Scalar)*arrays.nparticles);
		memcpy((void *)m_last_z, arrays.z, sizeof(Scalar)*arrays.nparticles);
		m_last_updated_tstep = timestep;
		}

	m_pdata->release();

	// don't worry about computing flops here, this is fast
	if (m_prof)
		m_prof->pop();	
	
	return result;
	}

/*! Generic statistics that apply to any neighbor list, like the number of updates,
	average number of neighbors, etc... are printed to stdout. Derived classes should 
	print any pertinient information they see fit to.
 */
void NeighborList::printStats()
	{
	cout << "-- Neighborlist stats:" << endl;
	cout << m_updates/m_pdata->getN() << " updates / " << m_forced_updates/m_pdata->getN() << " forced updates" << endl;

	// copy the list back from the device if we need to
	#ifdef USE_CUDA
	if (m_data_location == gpu)
		{
		deviceToHostCopy();
		m_data_location = cpugpu;
		}
	#endif

	// build some simple statistics of the number of neighbors
	unsigned int n_neigh_min = m_pdata->getN();
	unsigned int n_neigh_max = 0;
	Scalar n_neigh_avg = 0.0;

	for (unsigned int i = 0; i < m_pdata->getN(); i++)
		{
		unsigned int n_neigh = m_list[i].size();
		if (n_neigh < n_neigh_min)
			n_neigh_min = n_neigh;
		if (n_neigh > n_neigh_max)
			n_neigh_max = n_neigh;

		n_neigh_avg += Scalar(n_neigh);
		}
	
	// divide to get the average
	n_neigh_avg /= Scalar(m_pdata->getN());

	cout << "n_neigh_min: " << n_neigh_min << " / n_neigh_max: " << n_neigh_max << " / n_neigh_avg: " << n_neigh_avg << endl;
	}

/*! \post sets m_force_update to true if the ParticleData has been sorted since the last
	neighbor list update.
	*/
void NeighborList::checkForceUpdate()
	{
	if (m_last_updated_tstep <= m_pdata->getLastSortedTstep())
		m_force_update = true;
	}

/*! Loops through the particles and finds all of the particles \c j who's distance is less than
	\c r_cut \c + \c r_buff from particle \c i, includes either i < j or all neighbors depending 
	on the mode set by setStorageMode()
*/
void NeighborList::computeSimple()
	{
	// sanity check
	assert(m_pdata);
	
	// start up the profile
	if (m_prof)
		m_prof->push("Build list");
		
	// access the particle data
	const ParticleDataArraysConst& arrays = m_pdata->acquireReadOnly(); 
	// sanity check
	assert(arrays.x != NULL && arrays.y != NULL && arrays.z != NULL);
	
	// get a local copy of the simulation box too
	const BoxDim& box = m_pdata->getBox();
	// sanity check
	assert(box.xhi > box.xlo && box.yhi > box.ylo && box.zhi > box.zlo);
	
	// simple algorithm follows:
	
	// start by creating a temporary copy of r_cut sqaured
	Scalar rmaxsq = (m_r_cut + m_r_buff) * (m_r_cut + m_r_buff);	 
	
	// precalculate box lenghts
	Scalar Lx = box.xhi - box.xlo;
	Scalar Ly = box.yhi - box.ylo;
	Scalar Lz = box.zhi - box.zlo;
	Scalar Lx2 = Lx / Scalar(2.0);
	Scalar Ly2 = Ly / Scalar(2.0);
	Scalar Lz2 = Lz / Scalar(2.0);


	// start by clearing the entire list
	for (unsigned int i = 0; i < arrays.nparticles; i++)
		m_list[i].clear();

	// now we can loop over all particles in n^2 fashion and build the list
	unsigned int n_neigh = 0;
	for (unsigned int i = 0; i < arrays.nparticles; i++)
		{
		Scalar xi = arrays.x[i];
		Scalar yi = arrays.y[i];
		Scalar zi = arrays.z[i]; 
		const ExcludeList &excludes = m_exclusions[arrays.tag[i]];		
		
		// for each other particle with i < j
		for (unsigned int j = i + 1; j < arrays.nparticles; j++)
			{
			// early out if these are excluded pairs
			if (excludes.e1 == arrays.tag[j] || excludes.e2 == arrays.tag[j] || 
				excludes.e3 == arrays.tag[j] || excludes.e4 == arrays.tag[j])
				continue;

			// calculate dr
			Scalar dx = arrays.x[j] - xi;
			Scalar dy = arrays.y[j] - yi;
			Scalar dz = arrays.z[j] - zi;
			
			// if the vector crosses the box, pull it back
			if (dx >= Lx2)
				dx -= Lx;
			else
			if (dx < -Lx2)
				dx += Lx;
			
			if (dy >= Ly2)
				dy -= Ly;
			else
			if (dy < -Ly2)
				dy += Ly;
			
			if (dz >= Lz2)
				dz -= Lz;
			else
			if (dz < -Lz2)
				dz += Lz;
	
			// sanity check
			assert(dx >= box.xlo && dx < box.xhi);
			assert(dy >= box.ylo && dx < box.yhi);
			assert(dz >= box.zlo && dx < box.zhi);
			
			// now compare rsq to rmaxsq and add to the list if it meets the criteria
			Scalar rsq = dx*dx + dy*dy + dz*dz;
			if (rsq < rmaxsq)
				{
				if (m_storage_mode == full)
					{
					n_neigh += 2;
					m_list[i].push_back(j);
					m_list[j].push_back(i);
					}
				else
					{
					n_neigh += 1;
					m_list[i].push_back(j);
					}
				}
			}
		}

	m_pdata->release();

	// upate the profile
	// the number of evaluations made is the number of pairs which is = N(N-1)/2
	// each evalutation transfers 3*sizeof(Scalar) bytes for the particle access
	// and performs approximately 15 FLOPs
	// there are an additional N * 3 * sizeof(Scalar) accesses for the xj lookup
	uint64_t N = arrays.nparticles;
	if (m_prof)
		m_prof->pop(15*N*(N-1)/2, 3*sizeof(Scalar)*N*(N-1)/2 + N*3*sizeof(Scalar) + uint64_t(n_neigh)*sizeof(unsigned int));
	}
	
	
#ifdef USE_PYTHON

//! helper function for accessing an element of the neighbor list vector: python __getitem__
/*! \param nlist Neighbor list to extract a column from
	\param i item to extract
*/
const vector<unsigned int> & getNlistList(std::vector< std::vector<unsigned int> >* nlist, unsigned int i)
	{
	return (*nlist)[i];
	}
	
//! helper function for accessing an elemeng of the neighb rlist: python __getitem__
/*! \param list List to extract an item from
	\param i item to extract
*/
unsigned int getNlistItem(std::vector<unsigned int>* list, unsigned int i)
	{
	return (*list)[i];
	}
	
void export_NeighborList()
	{
	class_< std::vector< std::vector<unsigned int> > >("std_vector_std_vector_uint")
		.def("__len__", &std::vector< std::vector<unsigned int> >::size)
		.def("__getitem__", &getNlistList, return_internal_reference<>())
		;
		
	class_< std::vector<unsigned int> >("std_vector_uint")
		.def("__len__", &std::vector<unsigned int>::size)
		.def("__getitem__", &getNlistItem)
		;
	
	scope in_nlist = class_<NeighborList, boost::shared_ptr<NeighborList>, bases<Compute>, boost::noncopyable >
		("NeighborList", init< boost::shared_ptr<ParticleData>, Scalar, Scalar >())
		.def("setRCut", &NeighborList::setRCut)
		.def("setEvery", &NeighborList::setEvery)
		.def("getList", &NeighborList::getList, return_internal_reference<>())
		.def("setStorageMode", &NeighborList::setStorageMode)
		.def("addExclusion", &NeighborList::addExclusion)
		.def("forceUpdate", &NeighborList::forceUpdate)
		.def("estimateNNeigh", &NeighborList::estimateNNeigh)
		;
		
	enum_<NeighborList::storageMode>("storageMode")
		.value("half", NeighborList::half)
		.value("full", NeighborList::full)
	;
	}

#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// SSE optimized version
#ifdef USE_SSE
#ifdef __SSE__

/*! \param pdata Particle data the neighborlist is to compute neighbors for
	\param r_cut Cuttoff radius under which particles are considered neighbors
	\param r_buff Buffere radius around \a r_cut in which neighbors will be included
	This constructor has the same postconditions as the superclass: NeighborList
*/
NeighborListSSE::NeighborListSSE(ParticleData *pdata, Scalar r_cut, Scalar r_buff) : NeighborList(pdata, r_cut, r_buff)
	{
	}

// Need different SSE optmized functions for single and double precision
#if defined(__SSE2__) && !defined(SINGLE_PRECISION)
void NeighborListSSE::computeSimple()
	{
	// vector SSE version

	// sanity check
	assert(m_pdata);
		
	// start up the profile
	if (m_prof)
		m_prof->push("Build list");
		
	// access the particle data
	const ParticleDataArraysConst& arrays = m_pdata->acquireReadOnly(); 
	// sanity check
	assert(arrays.x != NULL && arrays.y != NULL && arrays.z != NULL);
	
	// get a local copy of the simulation box too
	const BoxDim& box = m_pdata->getBox();
	// sanity check
	assert(box.xhi > box.xlo && box.yhi > box.ylo && box.zhi > box.zlo);	
	
	// simple algorithm follows that has been modified to work with SSE intrinsics
	
	// start by putting a bunch of constants into SSE registers
	Scalar rmaxsq_scalar = (m_r_cut + m_r_buff) * (m_r_cut + m_r_buff);	  
	__m128d rmaxsq = _mm_set1_pd(rmaxsq_scalar);
	
	// precalculate box lenghts
	Scalar Lx_scalar = box.xhi - box.xlo;
	Scalar Ly_scalar = box.yhi - box.ylo;
	Scalar Lz_scalar = box.zhi - box.zlo;
	// precalculate box length over 2
	Scalar Lx2_scalar = Lx_scalar/2.0;
	Scalar Ly2_scalar = Ly_scalar/2.0;
	Scalar Lz2_scalar = Lz_scalar/2.0;
	// precalculate negative box length over 2 
	Scalar Lx2n_scalar = -Lx_scalar/2.0;
	Scalar Ly2n_scalar = -Ly_scalar/2.0;
	Scalar Lz2n_scalar = -Lz_scalar/2.0;

	// store all of those values into SSE registers. Both the upper and lower
	// values of these registers are set to the same value, as all particles 
	// coords will be compared to the same box lengths later
	__m128d Lx = _mm_set1_pd(Lx_scalar);
	__m128d Ly = _mm_set1_pd(Ly_scalar);
	__m128d Lz = _mm_set1_pd(Lz_scalar);

	__m128d Lx2 = _mm_set1_pd(Lx2_scalar);
	__m128d Lx2n = _mm_set1_pd(Lx2n_scalar);

	__m128d Ly2 = _mm_set1_pd(Ly2_scalar);
	__m128d Ly2n = _mm_set1_pd(Ly2n_scalar);

	__m128d Lz2 = _mm_set1_pd(Lz2_scalar);
	__m128d Lz2n = _mm_set1_pd(Lz2n_scalar);
	
	// start by clearing the entire list
	for (unsigned int i = 0; i < arrays.nparticles; i++)
		m_list[i].clear();

	// now we can loop over all particles in n^2 fashion and build the list
	unsigned int n_neigh = 0;
	for (unsigned int i = 0; i < arrays.nparticles; i++)
		{
		// load in particle i: we are going to compare particle i against all particles
		// j in a vector vashion. So, xi,yi,zi store particle i's coords in both the upper and lower 
		// parts of the register
		__m128d xi, yi, zi;

		xi = _mm_load1_pd(&arrays.x[i]);
		yi = _mm_load1_pd(&arrays.y[i]);
		zi = _mm_load1_pd(&arrays.z[i]); 
		const ExcludeList &excludes = m_exclusions[arrays.tag[i]]; //< for checking for exclusions
				
		// for each other particle with i < j, two at a time
		for (unsigned int j = i + 1; j < arrays.nparticles; j+=2)
			{
			__m128d dx, dy, dz;
			
			// load particles j and j+1 into the two slots in the vector register
			dx = _mm_loadu_pd(&arrays.x[j]);
			dy = _mm_loadu_pd(&arrays.y[j]);
			dz = _mm_loadu_pd(&arrays.z[j]);
			
			// calculate dx,dy,dz to the two particles j and j+1 in one instruction
			dx = _mm_sub_pd(dx,xi);
			dy = _mm_sub_pd(dy,yi);
			dz = _mm_sub_pd(dz,zi);
			
			// if the vector crosses the box, pull it back
			
			// doing this with if statements will ruin performance
			// we avoid this in a simple manner: calculate a mask that 
			//    indicates if the vector component is less than -Lx/2.0
			// mask L with that mask generating a value of 0 if the particle's coord is
			//    greater than -Lx/2.0 or a value of L if the particle's coord is less than -Lx/2.
			// Then, add this masked value to the component. These are all done in a vector fashion
			//    computing for two particles at a time.
			// Similarly handle the case where the vector is greather than Lx/2.0
			__m128d mask1x, mask2x, masked_L1x, masked_L2x;
			mask1x = _mm_cmpge_pd(dx, Lx2);
			mask2x = _mm_cmple_pd(dx, Lx2n);
			//if (dx >= box.xhi)
			//	dx -= Lx;
			masked_L1x = _mm_and_pd(mask1x, Lx);
			//if (dx < box.xlo)
				//dx += Lx;
			masked_L2x = _mm_and_pd(mask2x, Lx);
			masked_L1x = _mm_sub_pd(masked_L2x, masked_L1x);
			dx = _mm_add_pd(dx, masked_L1x);
		
			// see the description for Lx
			__m128d mask1y, mask2y, masked_L1y, masked_L2y;
			mask1y = _mm_cmpge_pd(dy, Ly2);
			mask2y = _mm_cmple_pd(dy, Ly2n);
			//if (dy >= box.yhi)
			//	dy -= Ly;
			masked_L1y = _mm_and_pd(mask1y, Ly);
			//if (dy < box.ylo)
				//dy += Ly;
			masked_L2y = _mm_and_pd(mask2y, Ly);
			masked_L1y = _mm_sub_pd(masked_L2y, masked_L1y);
			dy = _mm_add_pd(dy, masked_L1y);
			
			__m128d mask1z, mask2z, masked_L1z, masked_L2z;
			mask1z = _mm_cmpge_pd(dz, Lz2);
			mask2z = _mm_cmple_pd(dz, Lz2n);
			//if (dz >= box.zhi)
			//	dz -= Lz;
			masked_L1z = _mm_and_pd(mask1z, Lz);
	//		if (dz < box.zlo)
	//			dz += Lz;					
			masked_L2z = _mm_and_pd(mask2z, Lz);
			masked_L1z = _mm_sub_pd(masked_L2z, masked_L1z);
			dz = _mm_add_pd(dz, masked_L1z);

			// compute rsq, two particles at a time
			dx = _mm_mul_pd(dx,dx);
			dy = _mm_mul_pd(dy,dy);
			dz = _mm_mul_pd(dz,dz);

			// now compare rsq to rmaxsq and add to the list if it meets the criteria
			__m128d rsq;
			rsq = _mm_add_pd(dx,dy);
			rsq = _mm_add_pd(rsq,dz);
			__m128d mask = _mm_cmple_pd(rsq, rmaxsq);
			
			// now we can't do the rest with vector instructions so create a mask 
			// that has bit 1 set if rsq < rmaxsq for particle j
			// and has bit 2 set if rsq < rmaxsq for particle j+1
			// handle adding those particles to the list independantly
			int test = _mm_movemask_pd(mask);
			if (test & 1)
				{
				// if not excluded
				if (!(excludes.e1 == arrays.tag[j] || excludes.e2 == arrays.tag[j] || 
					excludes.e3 == arrays.tag[j] || excludes.e4 == arrays.tag[j]))
					{
					// add the particle

					if (m_storage_mode == full)
						{
						n_neigh += 2;
						m_list[i].push_back(j);
						m_list[j].push_back(i);
						}
					else
						{
						n_neigh += 1;
						m_list[i].push_back(j);
						}
					}
				}
			if (test & 2 && j+1 < arrays.nparticles)
				{
				// if not excluded
				if (!(excludes.e1 == arrays.tag[j+1] || excludes.e2 == arrays.tag[j+1] || 
					excludes.e3 == arrays.tag[j+1] || excludes.e4 == arrays.tag[j+1]))
					{
					// add the particle

					if (m_storage_mode == full)
						{
						n_neigh += 2;
						m_list[i].push_back(j+1);
						m_list[j+1].push_back(i);
						}
					else
						{
						n_neigh += 1;
						m_list[i].push_back(j+1);
						}
					}
				}
			}
		}
		
	m_pdata->release();
		
	// upate the profile
	// the number of evaluations made is the number of pairs which is = N(N-1)/2
	// each evalutation transfers 3*sizeof(Scalar) bytes for the particle access
	// and performs approximately 27 FLOPs
	// there are an additional N * 3 * sizeof(Scalar) accesses for the xj lookup
	uint64_t N = arrays.nparticles;
	if (m_prof)
		m_prof->pop(27*N*(N-1)/2, 3*sizeof(Scalar)*N*(N-1)/2 + N*3*sizeof(Scalar) + uint64_t(n_neigh)*sizeof(unsigned int));
	}
#endif

#if defined(__SSE__) && defined(SINGLE_PRECISION)
void NeighborListSSE::computeSimple()
	{
	// vector SSE version

	// sanity check
	assert(m_pdata);
		
	// start up the profile
	if (m_prof)
		m_prof->push("Build list");
		
	// access the particle data
	const ParticleDataArraysConst& arrays = m_pdata->acquireReadOnly(); 
	// sanity check
	assert(arrays.x != NULL && arrays.y != NULL && arrays.z != NULL);
	
	// get a local copy of the simulation box too
	const BoxDim& box = m_pdata->getBox();
	// sanity check
	assert(box.xhi > box.xlo && box.yhi > box.ylo && box.zhi > box.zlo);	
	
	// simple algorithm follows that has been modified to work with SSE intrinsics
	
	// start by putting a bunch of constants into SSE registers
	Scalar rmaxsq_scalar = (m_r_cut + m_r_buff) * (m_r_cut + m_r_buff);	 
	__m128 rmaxsq = _mm_set_ps1(rmaxsq_scalar);
	
	// precalculate box lenghts
	Scalar Lx_scalar = box.xhi - box.xlo;
	Scalar Ly_scalar = box.yhi - box.ylo;
	Scalar Lz_scalar = box.zhi - box.zlo;


	// store all of those values into SSE registers. All four elements of these
	// registers are set to the same value, as all particles coords will be compared 
	// to the same box lengths later
	__m128 Lx = _mm_set_ps1(Lx_scalar);
	__m128 Ly = _mm_set_ps1(Ly_scalar);
	__m128 Lz = _mm_set_ps1(Lz_scalar);

	__m128 xhi = _mm_set_ps1(box.xhi);
	__m128 xlo = _mm_set_ps1(box.xlo);

	__m128 yhi = _mm_set_ps1(box.yhi);
	__m128 ylo = _mm_set_ps1(box.ylo);

	__m128 zhi = _mm_set_ps1(box.zhi);
	__m128 zlo = _mm_set_ps1(box.zlo);

	// start by clearing the entire list
	for (unsigned int i = 0; i < arrays.nparticles; i++)
		m_list[i].clear();

	// now we can loop over all particles in n^2 fashion and build the list
	unsigned int n_neigh = 0;
	for (unsigned int i = 0; i < arrays.nparticles; i++)
		{
		// load in particle i: we are going to compare particle i against all particles
		// j in a vector vashion. So, xi,yi,zi store particle i's coords in all 4 elements of the
		// sse register		
		__m128 xi, yi, zi;

		xi = _mm_load_ps1(&arrays.x[i]);
		yi = _mm_load_ps1(&arrays.y[i]);
		zi = _mm_load_ps1(&arrays.z[i]); 
		const ExcludeList &excludes = m_exclusions[arrays.tag[i]]; //< for checking for exclusions
				
		// for each other particle with i < j, four at a time
		for (unsigned int j = i + 1; j < arrays.nparticles; j+=4)
			{
			__m128 dx, dy, dz;
			// load particles j, j+1, j+2, and j+4 into the four slots in the vector register
			dx = _mm_loadu_ps(&arrays.x[j]);
			dy = _mm_loadu_ps(&arrays.y[j]);
			dz = _mm_loadu_ps(&arrays.z[j]);
			
			// calculate dx,dy,dz to these 4 particles all in one instruction
			dx = _mm_sub_ps(dx,xi);
			dy = _mm_sub_ps(dy,yi);
			dz = _mm_sub_ps(dz,zi);
			
			// if the vector crosses the box, pull it back
			// see the double precision implementation for an explanation of this method
			// it is the same here, except it is done on four values at a time

			//if (dx >= box.xhi)
			//	dx -= Lx;
			__m128 mask1x, mask2x, masked_L1x, masked_L2x;
			mask1x = _mm_cmpge_ps(dx, xhi);
			masked_L1x = _mm_and_ps(mask1x, Lx);
			//if (dx < box.xlo)
				//dx += Lx;
			mask2x = _mm_cmple_ps(dx, xlo);
			masked_L2x = _mm_and_ps(mask2x, Lx);
			masked_L1x = _mm_sub_ps(masked_L2x, masked_L1x);
			dx = _mm_add_ps(dx, masked_L1x);
			
			__m128 mask1y, mask2y, masked_L1y, masked_L2y;
			//if (dy >= box.yhi)
			//	dy -= Ly;
			mask1y = _mm_cmpge_ps(dy, yhi);
			masked_L1y = _mm_and_ps(mask1y, Ly);
			//if (dy < box.ylo)
				//dy += Ly;
			mask2y = _mm_cmple_ps(dy, ylo);
			masked_L2y = _mm_and_ps(mask2y, Ly);
			masked_L1y = _mm_sub_ps(masked_L2y, masked_L1y);
			dy = _mm_add_ps(dy, masked_L1y);
			
			__m128 mask1z, mask2z, masked_L1z, masked_L2z;
			//if (dz >= box.zhi)
			//	dz -= Lz;
			mask1z = _mm_cmpge_ps(dz, zhi);
			masked_L1z = _mm_and_ps(mask1z, Lz);
//			if (dz < box.zlo)
//				dz += Lz;					
			mask2z = _mm_cmple_ps(dz, zlo);
			masked_L2z = _mm_and_ps(mask2z, Lz);
			masked_L1z = _mm_sub_ps(masked_L2z, masked_L1z);
			dz = _mm_add_ps(dz, masked_L1z);

			// compute rsq
			dx = _mm_mul_ps(dx,dx);
			dy = _mm_mul_ps(dy,dy);
			dz = _mm_mul_ps(dz,dz);

			// now compare rsq to rmaxsq and add to the list if it meets the criteria
			__m128 rsq;
			rsq = _mm_add_ps(dx,dy);
			rsq = _mm_add_ps(rsq,dz);
			__m128 mask = _mm_cmple_ps(rsq, rmaxsq);
			
			// four comparisons were made at once, and we can no longer do vector instructions
			// create a bitmask with bit 1 set if j is to be added
			//                       bit 2 set if j+1 is to be added
			//                       bit 3 set if j+2 is to be added
			//                   and bit 4 set if j+3 is to be added
			// break out and add each one if it matches the criteria
			int test = _mm_movemask_ps(mask);
			if (test & 1)
				{
				// if not excluded
				if (!(excludes.e1 == arrays.tag[j] || excludes.e2 == arrays.tag[j] || 
					excludes.e3 == arrays.tag[j] || excludes.e4 == arrays.tag[j]))
					{
					// add the particle
					if (m_storage_mode == full)
						{
						n_neigh += 2;
						m_list[i].push_back(j);
						m_list[j].push_back(i);
						}
					else
						{
						n_neigh += 1;
						m_list[i].push_back(j);
						}
					}
				}
			if (test & 2 && j+1 < arrays.nparticles)
				{
				// if not excluded
				if (!(excludes.e1 == arrays.tag[j+1] || excludes.e2 == arrays.tag[j+1] || 
					excludes.e3 == arrays.tag[j+1] || excludes.e4 == arrays.tag[j+1]))
					{
					// add the particle
					if (m_storage_mode == full)
						{
						n_neigh += 2;
						m_list[i].push_back(j+1);
						m_list[j+1].push_back(i);
						}
					else
						{
						n_neigh += 1;
						m_list[i].push_back(j+1);
						}
					}
				}
			if (test & 4 && j+2 < arrays.nparticles)
				{
				// if not excluded
				if (!(excludes.e1 == arrays.tag[j+2] || excludes.e2 == arrays.tag[j+2] || 
					excludes.e3 == arrays.tag[j+2] || excludes.e4 == arrays.tag[j+2]))
					{
					// add the particle
					if (m_storage_mode == full)
						{
						n_neigh += 2;
						m_list[i].push_back(j+2);
						m_list[j+2].push_back(i);
						}
					else
						{
						n_neigh += 1;
						m_list[i].push_back(j+2);
						}
					}
				}
			if (test & 8 && j+3 < arrays.nparticles)
				{
				// if not excluded
				if (!(excludes.e1 == arrays.tag[j+3] || excludes.e2 == arrays.tag[j+3] || 
					excludes.e3 == arrays.tag[j+3] || excludes.e4 == arrays.tag[j+3]))
					{
					// add the particle

					if (m_storage_mode == full)
						{
						n_neigh += 2;
						m_list[i].push_back(j+3);
						m_list[j+3].push_back(i);
						}
					else
						{
						n_neigh += 1;
						m_list[i].push_back(j+3);
						}
					}
				}
			}
		}
		
	m_pdata->release();
		
	// upate the profile
	// the number of evaluations made is the number of pairs which is = N(N-1)/2
	// each evalutation transfers 3*sizeof(Scalar) bytes for the particle access
	// and performs approximately 27 FLOPs
	// there are an additional N * 3 * sizeof(Scalar) accesses for the xj lookup
	uint64_t N = arrays.nparticles;
	if (m_prof)
		m_prof->pop(27*N*(N-1)/2, 3*sizeof(Scalar)*N*(N-1)/2 + N*3*sizeof(Scalar) + uint64_t(n_neigh)*sizeof(unsigned int));
	}

#endif
#endif
#endif
