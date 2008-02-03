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

#ifdef USE_PYTHON
#include <boost/python.hpp>
using namespace boost::python;
#endif

#include <sstream>
#include <fstream>

#include "BinnedNeighborList.h"

/*! \file BinnedNeighborList.cc
	\brief Contains code for the BinnedNeighborList class
*/

#include <algorithm>

using namespace std;

/*! \param pdata Particle data the neighborlist is to compute neighbors for
	\param r_cut Cuttoff radius under which particles are considered neighbors
	\param r_buff Buffere radius around \a r_cut in which neighbors will be included
	
	\post NeighborList is initialized and the list memory has been allocated,
		but the list will not be computed until compute is called.
	\post The storage mode defaults to half
*/
BinnedNeighborList::BinnedNeighborList(boost::shared_ptr<ParticleData> pdata, Scalar r_cut, Scalar r_buff) : NeighborList(pdata, r_cut, r_buff)
	{
	}

/*! Updates the neighborlist if it has not yet been updated this times step
 	\param timestep Current time step of the simulation
*/
void BinnedNeighborList::compute(unsigned int timestep)
	{
	checkForceUpdate();
	// skip if we shouldn't compute this step
	if (!shouldCompute(timestep) && !m_force_update)
		return;

	if (m_prof)
		m_prof->push("Nlist");

	// update the list (if it needs it)
	if (needsUpdating(timestep))
		{
		updateBins();
		updateListFromBins();

		#ifdef USE_CUDA
		// after computing, the device now resides on the CPU
		m_data_location = cpu;
		#endif
		}

	if (m_prof)
		m_prof->pop();
	}

/*! \post \c m_bins, \c m_binned_x, \c m_binned_y, and \c m_binned_z are updated with the current particle 
	positions. \c m_binned_tag is updated with the particle tags
	Each array contains \c m_MX * \c m_My * \c m_Mz bins. These dimensions will change over time if the size
	of the box changes. The dimension of each bin along any axis is >= r_cut so that particles can only 
	possibly be neighbors of particles in nearest neighbor bins.

	Given a coordinate \c ib, \c jb, \c kb the index of that bin in the bin arrays can be found by
	\c ib*(m_My*m_Mz) + jb * m_Mz + kb

	m_bins[bin] contains a list of particle indices that exist in that bin
	m_binned_x[bin] contains a list of the particle coordinates (at the time updateBins() was last called) of all 
	the particles in that bin. This is used to improve cache coherency in the next step of the computation.
*/
void BinnedNeighborList::updateBins()
	{
	assert(m_pdata);

	// start up the profile
	if (m_prof)
		m_prof->push("Bin");

	// acquire the particle data
	ParticleDataArraysConst arrays = m_pdata->acquireReadOnly();
	
	// calculate the bin dimensions
	const BoxDim& box = m_pdata->getBox();
	assert(box.xhi > box.xlo && box.yhi > box.ylo && box.zhi > box.zlo);	
	m_Mx = (unsigned int)((box.xhi - box.xlo) / (m_r_cut + m_r_buff));
	m_My = (unsigned int)((box.yhi - box.ylo) / (m_r_cut + m_r_buff));
	m_Mz = (unsigned int)((box.zhi - box.zlo) / (m_r_cut + m_r_buff));
	if (m_Mx == 0)
		m_Mx = 1;
	if (m_My == 0)
		m_My = 1;
	if (m_Mz == 0)
		m_Mz = 1;

	// make even bin dimensions
	Scalar binx = (box.xhi - box.xlo) / Scalar(m_Mx);
	Scalar biny = (box.yhi - box.ylo) / Scalar(m_Mx);
	Scalar binz = (box.zhi - box.zlo) / Scalar(m_Mx);

	// precompute scale factors to eliminate division in inner loop
	Scalar scalex = Scalar(1.0) / binx;
	Scalar scaley = Scalar(1.0) / biny;
	Scalar scalez = Scalar(1.0) / binz;

	// setup the memory arrays
	m_bins.resize(m_Mx*m_My*m_Mz);
	m_binned_x.resize(m_Mx*m_My*m_Mz);
	m_binned_y.resize(m_Mx*m_My*m_Mz);
	m_binned_z.resize(m_Mx*m_My*m_Mz);
	m_binned_tag.resize(m_Mx*m_My*m_Mz);

	// clear the bins
	for (unsigned int i = 0; i < m_Mx*m_My*m_Mz; i++)
		{
		m_bins[i].clear();
		m_binned_x[i].clear();
		m_binned_y[i].clear();
		m_binned_z[i].clear();
		m_binned_tag[i].clear();
		}

	// for each particle
	for (unsigned int n = 0; n < arrays.nparticles; n++)
		{
		// find the bin each particle belongs in
		unsigned int ib = (unsigned int)((arrays.x[n]-box.xlo)*scalex);
		unsigned int jb = (unsigned int)((arrays.y[n]-box.ylo)*scaley);
		unsigned int kb = (unsigned int)((arrays.z[n]-box.zlo)*scalez);

		// need to handle the case where the particle is exactly at the box hi
		if (ib == m_Mx)
			ib = 0;
		if (jb == m_My)
			jb = 0;
		if (kb == m_Mz)
			kb = 0;

		// sanity check
		assert(ib < (unsigned int)(m_Mx) && jb < (unsigned int)(m_My) && kb < (unsigned int)(m_Mz));

		// record its bin
		unsigned int bin = ib*(m_My*m_Mz) + jb * m_Mz + kb;

		m_bins[bin].push_back(n);
		m_binned_x[bin].push_back(arrays.x[n]);
		m_binned_y[bin].push_back(arrays.y[n]);
		m_binned_z[bin].push_back(arrays.z[n]);
		m_binned_tag[bin].push_back(arrays.tag[n]);
		}

	m_pdata->release();

	// update profile
	if (m_prof)
		m_prof->pop(6*arrays.nparticles, (3*sizeof(Scalar) + 28*sizeof(unsigned int))*arrays.nparticles);
	}

/*! \pre The bin arrays MUST be up to date. See updateBins()
	\post The neighborlist \c m_list is fully updated using the data in the bins. Every particle is compared
		against every particle in its bin and all of the nearest neighbor bins to see if this particle should
		be included in the neighbor list.

	\note This method uses a few neat tricks to improve performance. One is the use of the binned coordinates
		generated by updateBins(). If we were to read the coordinates from the particleData arrays by index in here,
		the random access pattern would harm performance because of cache misses. Since the coordinates are already
		loaded in from cache in updateBins(), it just dumps them into a bin array. That way, updateListFromBins()
		can read sequentially through memory without cache misses. There is a slight cost to the time it takes updateBins()
		to run, but that time is make up for in updateListFromBins(); Tests show an overall > 25% speed improvement in large
		systems (64k particles) where cache misses are more likely. The improvement is smaller for smaller systems, but is still 
		positive.
*/
void BinnedNeighborList::updateListFromBins()
	{
	// note: much of this code is copied directly from the base neighbor class and modified to 
	// use the bins instead of looping over every particle

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
	
	// start by creating a temporary copy of r_cut sqaured
	Scalar rmaxsq = (m_r_cut + m_r_buff) * (m_r_cut + m_r_buff);	 
	
	// precalculate box lenghts
	Scalar Lx = box.xhi - box.xlo;
	Scalar Ly = box.yhi - box.ylo;
	Scalar Lz = box.zhi - box.zlo;
	
	// make even bin dimensions
	Scalar binx = (box.xhi - box.xlo) / Scalar(m_Mx);
	Scalar biny = (box.yhi - box.ylo) / Scalar(m_Mx);
	Scalar binz = (box.zhi - box.zlo) / Scalar(m_Mx);

	// precompute scale factors to eliminate division in inner loop
	Scalar scalex = Scalar(1.0) / binx;
	Scalar scaley = Scalar(1.0) / biny;
	Scalar scalez = Scalar(1.0) / binz;
	
	// start by clearing the entire list
	for (unsigned int i = 0; i < arrays.nparticles; i++)
		m_list[i].clear();

	// now we can loop over all particles in a binned fashion and build the list
	unsigned int n_neigh = 0;
	unsigned int n_calc = 0;
	for (unsigned int n = 0; n < arrays.nparticles; n++)
		{
		// compare all pairs of particles n,m that are in neighboring bins
		Scalar xn = arrays.x[n];
		Scalar yn = arrays.y[n];
		Scalar zn = arrays.z[n]; 
		const ExcludeList &excludes = m_exclusions[arrays.tag[n]];
		
		// find the bin each particle belongs in
		int ib = int((xn-box.xlo)*scalex);
		int jb = int((yn-box.ylo)*scaley);
		int kb = int((zn-box.zlo)*scalez);

		// now loop over all of the neighboring bins
		for (int i = ib-1; i <= ib+1; i++)
			for (int j = jb-1; j <= jb+1; j++)
				for (int k = kb-1; k <= kb+1; k++)
					{
					// handle periodic boundary conditions of the bins
					int a = i;
					if (a < 0)
						a = a + m_Mx;
					else
					if (a >= int(m_Mx))
						a = a-m_Mx;

					int b = j;
					if (b < 0)
						b = b + m_My;
					else
					if (b >= int(m_My))
						b = b-m_My;

					int c = k;
					if (c < 0)
						c = c + m_Mz;
					else
					if (c >= int(m_Mz))
						c = c-m_Mz;
					
					// now we can generate a bin index
					unsigned int bin = a*(m_My*m_Mz) + b * m_Mz + c;

					// for each other particle in the neighboring bins of i
					const vector<unsigned int>& bin_list = m_bins[bin];
					const vector<Scalar>& bin_list_x = m_binned_x[bin];
					const vector<Scalar>& bin_list_y = m_binned_y[bin];
					const vector<Scalar>& bin_list_z = m_binned_z[bin];
					const vector<unsigned int>& bin_list_tag = m_binned_tag[bin];
					
					const unsigned int bin_size = bin_list.size();
					// count up the number of calculations we make
					n_calc += bin_size;
					for (unsigned int k = 0; k < bin_size; k++)
						{
						// we need to consider the pair m,n for the neihgborlist 
						unsigned int m = bin_list[k];

						// skip self pair
						if (n == m)
							continue;

						// calculate dx
						Scalar dx = bin_list_x[k] - xn;
						Scalar dy = bin_list_y[k] - yn;
						Scalar dz = bin_list_z[k] - zn;
			
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

						// sanity check
						assert(dx >= box.xlo && dx <= box.xhi);
						assert(dy >= box.ylo && dx <= box.yhi);
						assert(dz >= box.zlo && dx <= box.zhi);
			
						// now compare rsq to rmaxsq and add to the list if it meets the criteria
						Scalar rsq = dx*dx + dy*dy + dz*dz;
						if (rsq < rmaxsq)
							{
							// if not excluded
							if (!(excludes.e1 == bin_list_tag[k] || excludes.e2 == bin_list_tag[k] || 
							excludes.e3 == bin_list_tag[k] || excludes.e4 == bin_list_tag[k]))
								{
								// add the particle
								if (m_storage_mode == full || n < m)
									{
									n_neigh++;
									m_list[n].push_back(m);
									}
								}
							}
						}
					}
		}
		
	// sort the particles
	// this doesn't really NEED to be done, but when the particle data sort is implemented, it will
	// improve the cache coherency greatly, plus it only adds a 15% performance penalty
	// the development of SFCPACK has lessened the need for sorting, don't do it for
	// performance reasons
	/*for (unsigned int i = 0; i < m_pdata->getN(); i++)
		sort(m_list[i].begin(), m_list[i].end());*/

	m_pdata->release();

	// upate the profile
	// the number of evaluations made is the number of pairs which is n_calc (counted)
	// each evalutation transfers 3*sizeof(Scalar) bytes for the particle access
	// and 1*sizeof(unsigned int) bytes for the index access
	// and performs approximately 15 FLOPs
	// there are an additional N * 3 * sizeof(Scalar) accesses for the xj lookup
	// and n_neigh*sizeof(unsigned int) memory writes for the neighborlist
	uint64_t N = arrays.nparticles;
	if (m_prof)
		m_prof->pop(15*n_calc, 3*sizeof(Scalar)*n_calc + sizeof(unsigned int)*n_calc + N*3*sizeof(Scalar) + uint64_t(n_neigh)*sizeof(unsigned int));
	
	}

/*! Base class statistics are printed, along with statistics on how many particles are in bins
*/	
void BinnedNeighborList::printStats()
	{
	NeighborList::printStats();
	
	int min_b = INT_MAX;
	int max_b = 0;
	Scalar avg_b = 0.0;
	
	for (unsigned int i = 0; i < m_bins.size(); i++)
		{
		int s = m_bins[i].size();
		if (s < min_b)
			min_b = s;
		if (s > max_b)
			max_b = s;
		avg_b += Scalar(s);
		}
	avg_b /= Scalar(m_bins.size());
	
	cout << "bins_min: " << min_b << " / bins_max: " << max_b << " / bins_avg: " << avg_b << endl;
	}
	
	
#ifdef USE_PYTHON
void export_BinnedNeighborList()
	{
	class_<BinnedNeighborList, boost::shared_ptr<BinnedNeighborList>, bases<NeighborList>, boost::noncopyable >
		("BinnedNeighborList", init< boost::shared_ptr<ParticleData>, Scalar, Scalar >())
		;
	}
#endif
