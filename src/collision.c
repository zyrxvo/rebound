/**
 * @file 	collisions.c
 * @brief 	Direct collision search, O(N^2).
 * @author 	Hanno Rein <hanno@hanno-rein.de>
 *
 * @details 	This is the crudest implementation of a collision search
 * and checks every pair of particles. It is only useful very small 
 * particle numbers (N<~100) as it scales as O(N^2). 
 *
 * A collision is defined as an overlap between two particles. This
 * is only an approximation and works only if the timestep is small
 * enough. More precisely, dt << v / Rp, where v is the typical velocity
 * and Rp the radius of a particle. Furthermore, particles must be 
 * approaching each other at the time when they overlap. 
 * 
 * 
 * @section LICENSE
 * Copyright (c) 2011 Hanno Rein, Shangfei Liu
 *
 * This file is part of rebound.
 *
 * rebound is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * rebound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with rebound.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include "particle.h"
#include "collision.h"
#include "rebound.h"
#include "boundary.h"
#include "tree.h"

#ifdef MPI
#error COLLISIONS_DIRECT not yet compatible with MPI
#endif

void tree_get_nearest_neighbour_in_cell(struct Rebound* const r, struct Ghostbox gb, struct Ghostbox gbunmod, int ri, double p1_r,  double* nearest_r2, struct collision* collision_nearest, struct cell* c);

void collisions_search(struct Rebound* const r){
	const int N = r->N;
	const struct Particle* const particles = r->particles;
	switch (r->collision){
		case RB_CT_NONE:
		break;
		case RB_CT_DIRECT:
		{
			// Loop over ghost boxes, but only the inner most ring.
			int nghostxcol = (r->nghostx>1?1:r->nghostx);
			int nghostycol = (r->nghosty>1?1:r->nghosty);
			int nghostzcol = (r->nghostz>1?1:r->nghostz);
			for (int gbx=-nghostxcol; gbx<=nghostxcol; gbx++){
			for (int gby=-nghostycol; gby<=nghostycol; gby++){
			for (int gbz=-nghostzcol; gbz<=nghostzcol; gbz++){
				// Loop over all particles
				for (int i=0;i<N;i++){
					struct Particle p1 = particles[i];
					struct Ghostbox gborig = boundary_get_ghostbox(r, gbx,gby,gbz);
					struct Ghostbox gb = gborig;
					// Precalculate shifted position 
					gb.shiftx += p1.x;
					gb.shifty += p1.y;
					gb.shiftz += p1.z;
					gb.shiftvx += p1.vx;
					gb.shiftvy += p1.vy;
					gb.shiftvz += p1.vz;
					// Loop over all particles again
					for (int j=0;j<N;j++){
						// Do not collide particle with itself.
						if (i==j) continue;
						struct Particle p2 = particles[j];
						double dx = gb.shiftx - p2.x; 
						double dy = gb.shifty - p2.y; 
						double dz = gb.shiftz - p2.z; 
						double sr = p1.r + p2.r; 
						double r2 = dx*dx+dy*dy+dz*dz;
						// Check if particles are overlapping 
						if (r2>sr*sr) continue;	
						double dvx = gb.shiftvx - p2.vx; 
						double dvy = gb.shiftvy - p2.vy; 
						double dvz = gb.shiftvz - p2.vz; 
						// Check if particles are approaching each other
						if (dvx*dx + dvy*dy + dvz*dz >0) continue; 
						// Add particles to collision array.
						if (r->collisions_NMAX<=r->collisions_N){
							// Allocate memory if there is no space in array.
							// Doing it in chunks of 32 to avoid having to do it too often.
							r->collisions_NMAX += 32;
							r->collisions = realloc(r->collisions,sizeof(struct collision)*r->collisions_NMAX);
						}
						r->collisions[r->collisions_N].p1 = i;
						r->collisions[r->collisions_N].p2 = j;
						r->collisions[r->collisions_N].gb = gborig;
						r->collisions_N++;
					}
				}
			}
			}
			}
		}
		break;
		case RB_CT_TREE:
		{
			// Update and simplify tree. 
			// Prepare particles for distribution to other nodes. 
			tree_update(r);          

#ifdef MPI
			// Distribute particles and add newly received particles to tree.
			communication_mpi_distribute_particles();
			
			// Prepare essential tree (and particles close to the boundary needed for collisions) for distribution to other nodes.
			tree_prepare_essential_tree_for_collisions();

			// Transfer essential tree and particles needed for collisions.
			communication_mpi_distribute_essential_tree_for_collisions();
#endif // MPI

			// Loop over ghost boxes, but only the inner most ring.
			int nghostxcol = (r->nghostx>1?1:r->nghostx);
			int nghostycol = (r->nghosty>1?1:r->nghosty);
			int nghostzcol = (r->nghostz>1?1:r->nghostz);
			const struct Particle* const particles = r->particles;
			const int N = r->N;
			// Loop over all particles
#pragma omp parallel for schedule(guided)
			for (int i=0;i<N;i++){
				struct Particle p1 = particles[i];
				struct  collision collision_nearest;
				collision_nearest.p1 = i;
				collision_nearest.p2 = -1;
				double p1_r = p1.r;
				double nearest_r2 = r->boxsize_max*r->boxsize_max/4.;
				// Loop over ghost boxes.
				for (int gbx=-nghostxcol; gbx<=nghostxcol; gbx++){
				for (int gby=-nghostycol; gby<=nghostycol; gby++){
				for (int gbz=-nghostzcol; gbz<=nghostzcol; gbz++){
					// Calculated shifted position (for speedup). 
					struct Ghostbox gb = boundary_get_ghostbox(r, gbx,gby,gbz);
					struct Ghostbox gbunmod = gb;
					gb.shiftx += p1.x; 
					gb.shifty += p1.y; 
					gb.shiftz += p1.z; 
					gb.shiftvx += p1.vx; 
					gb.shiftvy += p1.vy; 
					gb.shiftvz += p1.vz; 
					// Loop over all root boxes.
					for (int ri=0;ri<r->root_n;ri++){
						struct cell* rootcell = r->tree_root[ri];
						if (rootcell!=NULL){
							tree_get_nearest_neighbour_in_cell(r, gb, gbunmod,ri,p1_r,&nearest_r2,&collision_nearest,rootcell);
						}
					}
				}
				}
				}
				// Continue if no collision was found
				if (collision_nearest.p2==-1) continue;
			}
		}
		break;
		default:
			fprintf(stderr,"\n\033[1mError!\033[0m Collision routine not implemented.\n");
			exit(EXIT_FAILURE);
	}

	// randomize
	for (int i=0;i<r->collisions_N;i++){
		int new = rand()%r->collisions_N;
		struct collision c1 = r->collisions[i];
		r->collisions[i] = r->collisions[new];
		r->collisions[new] = c1;
	}
	// Loop over all collisions previously found in collisions_search().
	for (int i=0;i<r->collisions_N;i++){
		// Resolve collision
		r->collision_resolve(r, r->collisions[i]);
	}
	// Mark all collisions as resolved.
	r->collisions_N=0;
}



/**
 * Find the nearest neighbour in a cell or its daughters.
 * The function only returns a positive result if the particles
 * are overlapping. Thus, the name nearest neighbour is not
 * exactly true.
 * @param gb (Shifted) position and velocity of the particle.
 * @param ri Index of the root box currently being searched in.
 * @param p1_r Radius of the particle (this is not in gb).
 * @param nearest_r2 Pointer to the nearest neighbour found so far.
 * @param collision_nearest Pointer to the nearest collision found so far.
 * @param c Pointer to the cell currently being searched in.
 */
void tree_get_nearest_neighbour_in_cell(struct Rebound* const r, struct Ghostbox gb, struct Ghostbox gbunmod, int ri, double p1_r, double* nearest_r2, struct collision* collision_nearest, struct cell* c){
	const struct Particle* const particles = r->particles;
	if (c->pt>=0){ 	
		// c is a leaf node
		int condition 	= 1;
#ifdef MPI
		int isloc	= 1 ;
		isloc = communication_mpi_rootbox_is_local(ri);
		if (isloc==1){
#endif // MPI
			/**
			 * If this is a local cell, make sure particle is not colliding with itself.
			 * If this is a remote cell, the particle number might be the same, even for 
			 * different particles. 
			 * @TODO This can probably be written in a cleaner way.
			 */
			condition = (c->pt != collision_nearest->p1);
#ifdef MPI
		}
#endif // MPI
		if (condition){
			struct Particle p2;
#ifdef MPI
			if (isloc==1){
#endif // MPI
				p2 = particles[c->pt];
#ifdef MPI
			}else{
				int root_n_per_node = root_n/mpi_num;
				int proc_id = ri/root_n_per_node;
				p2 = particles_recv[proc_id][c->pt];
			}
#endif // MPI

			double dx = gb.shiftx - p2.x;
			double dy = gb.shifty - p2.y;
			double dz = gb.shiftz - p2.z;
			double r2 = dx*dx+dy*dy+dz*dz;
			// A closer neighbour has already been found 
			//if (r2 > *nearest_r2) return;
			double rp = p1_r+p2.r;
			// Particles are not overlapping 
			if (r2 > rp*rp) return;
			double dvx = gb.shiftvx - p2.vx;
			double dvy = gb.shiftvy - p2.vy;
			double dvz = gb.shiftvz - p2.vz;
			// Particles are not approaching each other
			if (dvx*dx + dvy*dy + dvz*dz >0) return;
			// Found a new nearest neighbour. Save it for later.
			*nearest_r2 = r2;
			collision_nearest->ri = ri;
			collision_nearest->p2 = c->pt;
			collision_nearest->gb = gbunmod;
			// Save collision in collisions array.
#pragma omp critical
			{
				if (r->collisions_NMAX<=r->collisions_N){
					r->collisions_NMAX += 32;
					r->collisions = realloc(r->collisions,sizeof(struct collision)*r->collisions_NMAX);
				}
				r->collisions[r->collisions_N] = *collision_nearest;
				r->collisions_N++;
			}
		}
	}else{		
		// c is not a leaf node
		double dx = gb.shiftx - c->x;
		double dy = gb.shifty - c->y;
		double dz = gb.shiftz - c->z;
		double r2 = dx*dx + dy*dy + dz*dz;
		double rp  = p1_r + r->max_radius[1] + 0.86602540378443*c->w;
		// Check if we need to decent into daughter cells
		if (r2 < rp*rp ){
			for (int o=0;o<8;o++){
				struct cell* d = c->oct[o];
				if (d!=NULL){
					tree_get_nearest_neighbour_in_cell(r, gb,gbunmod,ri,p1_r,nearest_r2,collision_nearest,d);
				}
			}
		}
	}
}






void collision_resolve_hardsphere(struct Rebound* const r, struct collision c){
#ifndef COLLISIONS_NONE
	struct Particle* const particles = r->particles;
	struct Particle p1 = particles[c.p1];
	struct Particle p2;
#ifdef MPI
	int isloc = communication_mpi_rootbox_is_local(c.ri);
	if (isloc==1){
#endif // MPI
		p2 = particles[c.p2];
#ifdef MPI
	}else{
		int root_n_per_node = root_n/mpi_num;
		int proc_id = c.ri/root_n_per_node;
		p2 = particles_recv[proc_id][c.p2];
	}
#endif // MPI
//	if (p1.lastcollision==t || p2.lastcollision==t) return;
	struct Ghostbox gb = c.gb;
	double x21  = p1.x + gb.shiftx  - p2.x; 
	double y21  = p1.y + gb.shifty  - p2.y; 
	double z21  = p1.z + gb.shiftz  - p2.z; 
	double rp   = p1.r+p2.r;
	double oldvyouter;
	if (x21>0){
	 	oldvyouter = p1.vy;
	}else{
		oldvyouter = p2.vy;
	}
	if (rp*rp < x21*x21 + y21*y21 + z21*z21) return;
	double vx21 = p1.vx + gb.shiftvx - p2.vx; 
	double vy21 = p1.vy + gb.shiftvy - p2.vy; 
	double vz21 = p1.vz + gb.shiftvz - p2.vz; 
	if (vx21*x21 + vy21*y21 + vz21*z21 >0) return; // not approaching
	// Bring the to balls in the xy plane.
	// NOTE: this could probabely be an atan (which is faster than atan2)
	double theta = atan2(z21,y21);
	double stheta = sin(theta);
	double ctheta = cos(theta);
	double vy21n = ctheta * vy21 + stheta * vz21;	
	double y21n = ctheta * y21 + stheta * z21;	
	
	// Bring the two balls onto the positive x axis.
	double phi = atan2(y21n,x21);
	double cphi = cos(phi);
	double sphi = sin(phi);
	double vx21nn = cphi * vx21  + sphi * vy21n;		

	// Coefficient of restitution
	double eps= r->collisions_coefficient_of_restitution_for_velocity(r, vx21nn);
	double dvx2 = -(1.0+eps)*vx21nn;
	double minr = (p1.r>p2.r)?p2.r:p1.r;
	double maxr = (p1.r<p2.r)?p2.r:p1.r;
	double mindv= minr*r->minimum_collision_velocity;
	double _r = sqrt(x21*x21 + y21*y21 + z21*z21);
	mindv *= 1.-(_r - maxr)/minr;
	if (mindv>maxr*r->minimum_collision_velocity)mindv = maxr*r->minimum_collision_velocity;
	if (dvx2<mindv) dvx2 = mindv;
	// Now we are rotating backwards
	double dvx2n = cphi * dvx2;		
	double dvy2n = sphi * dvx2;		
	double dvy2nn = ctheta * dvy2n;	
	double dvz2nn = stheta * dvy2n;	


	// Applying the changes to the particles.
#ifdef MPI
	if (isloc==1){
#endif // MPI
	const double p2pf = p1.m/(p1.m+p2.m);
	particles[c.p2].vx -=	p2pf*dvx2n;
	particles[c.p2].vy -=	p2pf*dvy2nn;
	particles[c.p2].vz -=	p2pf*dvz2nn;
	particles[c.p2].lastcollision = r->t;
#ifdef MPI
	}
#endif // MPI
	const double p1pf = p2.m/(p1.m+p2.m);
	particles[c.p1].vx +=	p1pf*dvx2n; 
	particles[c.p1].vy +=	p1pf*dvy2nn; 
	particles[c.p1].vz +=	p1pf*dvz2nn; 
	particles[c.p1].lastcollision = r->t;
		
	// Return y-momentum change
	if (x21>0){
		r->collisions_plog += -fabs(x21)*(oldvyouter-particles[c.p1].vy) * p1.m;
		r->collisions_Nlog ++;
	}else{
		r->collisions_plog += -fabs(x21)*(oldvyouter-particles[c.p2].vy) * p2.m;
		r->collisions_Nlog ++;
	}

#endif // COLLISIONS_NONE
}

double collisions_constant_coefficient_of_restitution_for_velocity(const struct Rebound* const r, double v){
	return r->coefficient_of_restitution;
}