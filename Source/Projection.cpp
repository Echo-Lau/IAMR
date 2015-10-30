
#include <winstd.H>

#include <Geometry.H>
#include <ParmParse.H>
#include <NavierStokes.H>
#include <NS_BC.H>
#include <BLProfiler.H>
#include <Projection.H>
#include <PROJECTION_F.H>
#include <NAVIERSTOKES_F.H>
#include <ProjOutFlowBC.H>

#include <MGT_Solver.H>
#include <stencil_types.H>
#include <mg_cpp_f.h>

#ifndef _NavierStokes_H_
enum StateType {State_Type=0, Press_Type};
#if (BL_SPACEDIM == 2)
enum StateNames  { Xvel=0, Yvel, Density};
#else
enum StateNames  { Xvel=0, Yvel, Zvel, Density};
#endif
#endif

#define DEF_LIMITS(fab,fabdat,fablo,fabhi)   \
const int* fablo = (fab).loVect();           \
const int* fabhi = (fab).hiVect();           \
Real* fabdat = (fab).dataPtr();

#define DEF_CLIMITS(fab,fabdat,fablo,fabhi)  \
const int* fablo = (fab).loVect();           \
const int* fabhi = (fab).hiVect();           \
const Real* fabdat = (fab).dataPtr();

#define DEF_BOX_LIMITS(box,boxlo,boxhi)   \
const int* boxlo = (box).loVect();           \
const int* boxhi = (box).hiVect();

const Real Projection::BogusValue = 1.e200;

//
// NOTE: the RegType array project_bc is now defined in NS_BC.H in iamrlib
//

namespace
{
    bool initialized = false;
}
//
// Set defaults for all static members in Initialize()!!!
//
int  Projection::P_code;
int  Projection::proj_2;
int  Projection::verbose;
Real Projection::proj_tol;
Real Projection::sync_tol;
Real Projection::proj_abs_tol;
int  Projection::add_vort_proj;
int  Projection::do_outflow_bcs;
int  Projection::rho_wgt_vel_proj;
int  Projection::make_sync_solvable;
Real Projection::divu_minus_s_factor;

static int hg_stencil = ND_DENSE_STENCIL;

namespace
{
#if MG_USE_HYPRE
    bool use_hypre_solve;
#endif

    bool benchmarking;
}


void
Projection::Initialize ()
{
    if (initialized) return;
    //
    // Set defaults here !!!
    //
    benchmarking                    = false;
    Projection::P_code              = 0;
    Projection::proj_2              = 1;
    Projection::verbose             = 0;
    Projection::proj_tol            = 1.0e-12;
    Projection::sync_tol            = 1.0e-8;
    Projection::proj_abs_tol        = 1.0e-16;
    Projection::add_vort_proj       = 0;
    Projection::do_outflow_bcs      = 1;
    Projection::rho_wgt_vel_proj    = 0;
    Projection::make_sync_solvable  = 0;
    Projection::divu_minus_s_factor = 0.0;

#if MG_USE_HYPRE
    use_hypre_solve = false;
#endif

    ParmParse pp("proj");

    pp.query("v",                   verbose);
    pp.query("Pcode",               P_code);
    pp.query("proj_2",              proj_2);
    pp.query("proj_tol",            proj_tol);
    pp.query("sync_tol",            sync_tol);
    pp.query("proj_abs_tol",        proj_abs_tol);
    pp.query("benchmarking",        benchmarking);
    pp.query("add_vort_proj",       add_vort_proj);
    pp.query("do_outflow_bcs",      do_outflow_bcs);
    pp.query("rho_wgt_vel_proj",    rho_wgt_vel_proj);
    pp.query("divu_minus_s_factor", divu_minus_s_factor);
    pp.query("make_sync_solvable",  make_sync_solvable);

    if (!proj_2) 
	BoxLib::Error("With new gravity and outflow stuff, must use proj_2");

    std::string stencil;

    if ( pp.query("stencil", stencil) )
    {
        if ( stencil == "cross" )
        {
            hg_stencil = ND_CROSS_STENCIL;
        }
        else if ( stencil == "full" || stencil == "dense")
        {
            hg_stencil = ND_DENSE_STENCIL;
        }
        else
        {
            BoxLib::Error("Must set proj.stencil to be cross, full or dense");
        }
    }

    BoxLib::ExecOnFinalize(Projection::Finalize);

    initialized = true;
}

void
Projection::Finalize ()
{
    initialized = false;
}

Projection::Projection (Amr*   _parent,
                        BCRec* _phys_bc, 
                        int    _do_sync_proj,
                        int    /*_finest_level*/, 
                        int    _radius_grow )
   :
    parent(_parent),
    LevelData(_parent->finestLevel()+1),
    radius_grow(_radius_grow), 
    radius(_parent->finestLevel()+1),
    anel_coeff(_parent->finestLevel()+1),
    phys_bc(_phys_bc), 
    do_sync_proj(_do_sync_proj)
{

    BL_ASSERT ( parent->finestLevel()+1 <= maxlev );

    Initialize();

    if (verbose && ParallelDescriptor::IOProcessor()) 
        std::cout << "Creating projector\n";

    for (int lev = 0; lev <= parent->finestLevel(); lev++)
       anel_coeff[lev] = 0;
}

Projection::~Projection ()
{
    if (verbose && ParallelDescriptor::IOProcessor()) 
        std::cout << "Deleting projector\n";
}

//
// Install a level of the projection.
//

void
Projection::install_level (int                   level,
                           AmrLevel*             level_data,
                           Array< Array<Real> >* _radius)
{
    if (verbose && ParallelDescriptor::IOProcessor()) 
        std::cout << "Installing projector level " << level << '\n';

    int finest_level = parent->finestLevel();

    if (level > LevelData.size() - 1) 
    {
        LevelData.resize(finest_level+1);
        radius.resize(finest_level+1);
    }

    if (level > anel_coeff.size()-1) {
       anel_coeff.resize(level+1);
       anel_coeff[level] = 0;
    }

    LevelData.clear(level);
    LevelData.set(level, level_data);
    radius.clear(level);
    radius.set(level, _radius);
}

void
Projection::install_anelastic_coefficient (int                   level,
                                           Real                **_anel_coeff)
{
    if (verbose && ParallelDescriptor::IOProcessor()) 
        std::cout << "Installing anel_coeff into projector level " << level << '\n';
    if (level > anel_coeff.size()-1) 
       anel_coeff.resize(level+1);
    anel_coeff.set(level, _anel_coeff);
}

//
//  Perform a level projection in the advance function
//  Explanation of arguments to the level projector:
//
//  rho_half  contains rho^{n+1/2}
//  U_old  contains the u^n velocities
//  U_new  starts as u^*, is converted to (u^* - u^n)/dt,
//         becomes (u^{n+1} - u^n)/dt in the solver,
//         and is converted back to u^n+1 at the end
//  P_old  contains p^{n-1/2}
//  P_new  gets cleared, initialized to an intial guess for p^{n+1/2}
//         using coarse grid data if available,
//         becomes pressure update phi in the solver,
//         and then converted into final prssure p^{n+1/2}
//

void
Projection::level_project (int             level,
                           Real            time,
                           Real            dt,
                           Real            cur_pres_time,
                           Real            prev_pres_time,
                           const Geometry& geom, 
                           MultiFab&       U_old,
                           MultiFab&       U_new,
                           MultiFab&       P_old,
                           MultiFab&       P_new,
                           MultiFab*       rho_half, 
                           SyncRegister*   crse_sync_reg, 
                           SyncRegister*   fine_sync_reg,  
                           int             crse_dt_ratio,
                           int             iteration,
                           int             have_divu)
{
    BL_PROFILE("Projection::level_project()");

    if ( verbose && ParallelDescriptor::IOProcessor() )
	std::cout << "... level projector at level " << level << '\n';

    if (verbose && benchmarking) ParallelDescriptor::Barrier();

    const Real strt_time = ParallelDescriptor::second();
    //
    // old time velocity has bndry values already
    // must gen valid bndry data for new time velocity.
    // must fill bndry cells in pressure with computable values
    // even though they are not used in calculation.
    //
    U_old.setBndry(BogusValue,Xvel,BL_SPACEDIM);
    U_new.setBndry(BogusValue,Xvel,BL_SPACEDIM);
    P_old.setBndry(BogusValue);
    P_new.setBndry(BogusValue);

    MultiFab& S_old = LevelData[level].get_old_data(State_Type);
    MultiFab& S_new = LevelData[level].get_new_data(State_Type);
    
    Real prev_time = LevelData[level].get_state_data(State_Type).prevTime();
    Real curr_time = LevelData[level].get_state_data(State_Type).curTime();
    
    for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
    {
        LevelData[level].setPhysBoundaryValues(S_old[mfi],State_Type,prev_time,
                                               Xvel,Xvel,BL_SPACEDIM);
        LevelData[level].setPhysBoundaryValues(S_new[mfi],State_Type,curr_time,
                                               Xvel,Xvel,BL_SPACEDIM);
    }

    const BoxArray& grids   = LevelData[level].boxArray();
    const BoxArray& P_grids = P_old.boxArray();

    NavierStokes* ns = dynamic_cast<NavierStokes*>(&parent->getLevel(level));
    BL_ASSERT(!(ns==0));

    //
    //  NOTE: IT IS IMPORTANT TO DO THE BOUNDARY CONDITIONS BEFORE
    //    MAKING UNEW HOLD U_t OR U/dt, BECAUSE UNEW IS USED IN
    //    CONSTRUCTING THE OUTFLOW BC'S.
    //
    // Set boundary values for P_new, to increment, if applicable
    // // Note: we don't need to worry here about using FillCoarsePatch because
    //       it will automatically use the "new dpdt" to interpolate,
    //       since once we get here at level > 0, we've already defined
    //       a new pressure at level-1.
    if (level != 0)
    {
	LevelData[level].FillCoarsePatch(P_new,0,cur_pres_time,Press_Type,0,1);
        if (!proj_2) 
            P_new.minus(P_old,0,1,0); // Care about nodes on box boundary
    }

    const int nGrow = (level == 0  ?  0  :  -1);
    for (MFIter P_newmfi(P_new); P_newmfi.isValid(); ++P_newmfi)
    {
        const int i = P_newmfi.index();

        P_new[P_newmfi].setVal(0.0,BoxLib::grow(P_new.box(i),nGrow),0,1);
    }

    //
    // Compute Ustar/dt + Gp                  for proj_2,
    //         (Ustar-Un)/dt for not proj_2 (ie the original).
    //
    // Compute DU/dt for proj_2,
    //         (DU-DU_old)/dt for not proj_2 (ie the original).
    //
    MultiFab *divusource = 0, *divuold = 0;

    if (have_divu)
    {
        divusource = ns->getDivCond(1,time+dt);
        if (!proj_2)
            divuold = ns->getDivCond(1,time);
    }

    const Real dt_inv = 1./dt;
    if (proj_2)
    {
        U_new.mult(dt_inv,0,BL_SPACEDIM,1);
        if (have_divu)
            divusource->mult(dt_inv,0,1,divusource->nGrow());
    }
    else
    {
        for (MFIter U_newmfi(U_new); U_newmfi.isValid(); ++U_newmfi) 
        {
            const int i = U_newmfi.index();

            ConvertUnew(U_new[U_newmfi],U_old[U_newmfi],dt,U_new.box(i));
        } 

        if (have_divu)
        {
            divusource->minus(*divuold,0,1,divusource->nGrow());
            divusource->mult(dt_inv,0,1,divusource->nGrow());

            if (divu_minus_s_factor>0.0 && divu_minus_s_factor<=1.0)
            {
                BoxLib::Error("Check this code....not recently tested");
                //
                // Compute relaxation terms to account for approximate projection
                // add divu_old*divu...factor/dt to divusource.
                //
                const Real uoldfactor = divu_minus_s_factor*dt/parent->dtLevel(0);
                UpdateArg1(*divusource, uoldfactor/dt, *divuold, 1, grids, 1);
                //
                // add U_old*divu...factor/dt to U_new
                //
                UpdateArg1(U_new, uoldfactor/dt, U_old, BL_SPACEDIM, grids, 1);
            }
        }
    }
    delete divuold;

    if (proj_2)
    {
        MultiFab Gp(grids,BL_SPACEDIM,1);
        ns->getGradP(Gp, prev_pres_time);

	for (MFIter mfi(*rho_half); mfi.isValid(); ++mfi) {
          FArrayBox& Gpfab = Gp[mfi];
          const FArrayBox& rhofab = (*rho_half)[mfi];
      
	  for (int i = 0; i < BL_SPACEDIM; i++) {
	    Gpfab.divide(rhofab,0,i,1);
	  }
      
	  U_new[mfi].plus(Gpfab,0,0,BL_SPACEDIM);
	}
    }

    //
    // Outflow uses appropriately constructed "U_new" and "divusource"
    //   so make sure this call comes after those are set,
    //   but before fields are scaled by r or rho is set to 1/rho.
    //
    Real gravity = ns->getGravity();
    if (OutFlowBC::HasOutFlowBC(phys_bc) && (have_divu || std::fabs(gravity) > 0.0) 
                                         && do_outflow_bcs) 
    {
        MultiFab* phi[maxlev] = {0};
        phi[level] = &LevelData[level].get_new_data(Press_Type);

        MultiFab* Vel_ML[maxlev] = {0};
        Vel_ML[level] = &U_new;

        MultiFab* Divu_ML[maxlev] = {0};
        Divu_ML[level] = divusource;

        MultiFab* Rho_ML[maxlev] = {0};
        Rho_ML[level] = rho_half;

        set_outflow_bcs(LEVEL_PROJ,phi,Vel_ML,Divu_ML,Rho_ML,level,level,have_divu);
    }

    //
    // Scale the projection variables.
    //
    rho_half->setBndry(BogusValue);
    scaleVar(LEVEL_PROJ,rho_half, 1, &U_new, level);
    //
    // Enforce periodicity of U_new and rho_half (i.e. coefficient of G phi)
    // *after* everything has been done to them.
    //
    EnforcePeriodicity(U_new,     BL_SPACEDIM, grids, geom);
    EnforcePeriodicity(*rho_half, 1,           grids, geom);
    //
    // Add the contribution from the un-projected V to syncregisters.
    //
    int is_rz = (Geometry::IsRZ() ? 1 : 0);

    MultiFab* vel[maxlev] = {0};
    MultiFab* phi[maxlev] = {0};
    MultiFab* sig[maxlev] = {0};

    vel[level] = &U_new;
    phi[level] = &P_new;

    BL_ASSERT( 1 == rho_half->nGrow());
    sig[level] = rho_half;

    //
    // Project
    //
    MultiFab* sync_resid_crse = 0;
    MultiFab* sync_resid_fine = 0;

    if (level < parent->finestLevel()) 
        sync_resid_crse = new MultiFab(P_grids,1,1);

    if (level > 0 && ((proj_2 && iteration == crse_dt_ratio) || !proj_2))
    {
        const int ngrow = parent->MaxRefRatio(level-1) - 1;
        sync_resid_fine = new MultiFab(P_grids,1,ngrow);
    }

    if (!have_divu) 
    {
        MultiFab* rhs[maxlev] = {0};
        PArray<MultiFab> rhnd;
        doNodalProjection(level, 1, vel, phi, sig, rhs, rhnd, proj_tol, proj_abs_tol, 
			  sync_resid_crse, sync_resid_fine);
    }
    else 
    {
        if (is_rz == 1)
            radMult(level,*divusource,0);
        const int nghost = 0;
        divusource->mult(-1.0,0,1,nghost); // FIXME: this doesn't touch the ghost cells?
	//                                    wqz. I don't think we need to.

        PArray<MultiFab> rhs_real(level+1);
        rhs_real.set(level, divusource);

	MultiFab* rhs_cc[maxlev] = {0};
	rhs_cc[level] = divusource;
        PArray<MultiFab> rhnd;
        doNodalProjection(level, 1, vel, phi, sig, rhs_cc, rhnd, proj_tol, proj_abs_tol,
			  sync_resid_crse, sync_resid_fine);
    }

    delete divusource;
    //
    // Note: this must occur *after* the projection has been done
    //       (but before the modified velocity has been copied back)
    //       because the SyncRegister routines assume the projection
    //       has been set up.
    //
    if (do_sync_proj)
    {
       if (level < parent->finestLevel())
       {
          //
          // Init sync registers between level and level+1.
          //
          const Real mult = 1.0;
          crse_sync_reg->CrseInit(sync_resid_crse,geom,mult);
       }
       if (level > 0 && ((proj_2 && iteration == crse_dt_ratio) || !proj_2))
       {
          //
          // Increment sync registers between level and level-1.
          //
	 // invrat is 1/crse_dt_ratio for both proj_2 and !proj_2, but for different reasons.
	 // For !proj_2, this is because the fine residue is added to the sync register 
	 //    for each fine step.
	 // For proj_2, this is because the level projection works on U/dt, not dU/dt, 
	 //    and dt on the fine level is crse_dt_ratio times smaller than dt one the 
	 //    coarse level.
	  const Real invrat = 1.0/(double)crse_dt_ratio;
          const Geometry& crse_geom = parent->Geom(level-1);
          fine_sync_reg->FineAdd(sync_resid_fine,geom,crse_geom,phys_bc,invrat);
       }
    }
    delete sync_resid_crse;
    delete sync_resid_fine;

    //
    // Reset state + pressure data.
    //
    // Unscale level projection variables.
    //
    rescaleVar(LEVEL_PROJ,rho_half, 1, &U_new, level);
    //
    // Put U_new back to "normal"; subtract U_old*divu...factor/dt from U_new
    //
    if (!proj_2 && divu_minus_s_factor>0.0 && divu_minus_s_factor<=1.0 && have_divu) 
    {
        const Real uoldfactor = -divu_minus_s_factor*dt/parent->dtLevel(0);
        UpdateArg1(U_new, uoldfactor/dt, U_old, BL_SPACEDIM, grids, 1);
    }
    //
    // Convert U back to a velocity, and phi into p^n+1/2.
    //
    if (proj_2) 
    {
        //
        // un = dt*un
        //
        U_new.mult(dt,0,BL_SPACEDIM,1);
    }
    else
    {
        //
        // un = uo+dt*un
        //
        UnConvertUnew(U_old, dt, U_new, grids);
    }

    if (!proj_2) 
        AddPhi(P_new, P_old);             // pn = pn + po

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
        {
            std::cout << "Projection::level_project(): lev: "
                      << level
                      << ", time: " << run_time << '\n';
        }
    }
}

//
// SYNC_PROJECT
//

void
Projection::syncProject (int             c_lev,
                         MultiFab&       pres,
                         MultiFab&       vel,
                         MultiFab*       rho_half,
                         MultiFab*       Vsync,
                         MultiFab&       phi,
                         SyncRegister*   rhs_sync_reg,
                         SyncRegister*   crsr_sync_reg,
                         const BoxArray& sync_boxes,
                         const Geometry& geom,
                         const Real*     dx,
                         Real            dt_crse,
                         int             crse_iteration,
                         int             crse_dt_ratio)
{
    BL_PROFILE("Projection::syncProject()");

    if (verbose && ParallelDescriptor::IOProcessor()) 
    {
        std::cout << "SyncProject: level = "
                  << c_lev
                  << " correction to level "
                  << parent->finestLevel() << '\n';
    }

    if (verbose && benchmarking) ParallelDescriptor::Barrier();

    const Real strt_time = ParallelDescriptor::second();
    //
    // Gather data.
    //
    const BoxArray& grids   = LevelData[c_lev].boxArray();
    const BoxArray& P_grids = pres.boxArray();
    MultiFab& sig = *rho_half;

    PArray<MultiFab> rhnd(1, PArrayManage);
    rhnd.set(0, new MultiFab(P_grids,1,1));
    rhs_sync_reg->InitRHS(rhnd[0],geom,phys_bc);

    phi.setVal(0);

    sig.setBndry(BogusValue);
    //
    // Scale sync projection variables.
    //
    scaleVar(SYNC_PROJ,&sig,1,Vsync,c_lev);
    //
    // If periodic, copy into periodic translates of Vsync.
    //
    EnforcePeriodicity(*Vsync, BL_SPACEDIM, grids, geom);

    MultiFab *phis[maxlev] = {0};
    MultiFab* vels[maxlev] = {0};
    MultiFab* sigs[maxlev] = {0};
    MultiFab* rhss[maxlev] = {0};
    phis[c_lev] = &phi;
    vels[c_lev] = Vsync;
    sigs[c_lev] = &sig;

    //
    //  PROJECT
    //  if use_u = 0, then solves DGphi = RHS
    //  if use_u = 1, then solves DGphi = RHS + DV
    //  both return phi and (V-Gphi) as V
    //
    MultiFab* sync_resid_crse = 0;
    MultiFab* sync_resid_fine = 0;

    if (c_lev > 0 && (!proj_2 || crse_iteration == crse_dt_ratio))
    {
        const int ngrow = parent->MaxRefRatio(c_lev-1) - 1;
        sync_resid_fine = new MultiFab(P_grids,1,ngrow);
    }

    doNodalProjection(c_lev, 1, vels, phis, sigs, rhss, rhnd, sync_tol, proj_abs_tol,
		      sync_resid_crse, sync_resid_fine);

    //
    // If this sync project is not at level 0 then we need to account for
    // the changes made here in the level c_lev velocity in the sync registers
    // going into the level (c_lev-1) sync project.  Note that this must be
    // done before rho_half is scaled back.
    //
    if (c_lev > 0 && (!proj_2 || crse_iteration == crse_dt_ratio))
    {
        const Real invrat         = 1.0/(double)crse_dt_ratio;
        const Geometry& crsr_geom = parent->Geom(c_lev-1);
        crsr_sync_reg->CompAdd(sync_resid_fine,geom,crsr_geom,phys_bc,sync_boxes,invrat);
    }
    delete sync_resid_crse;
    delete sync_resid_fine;
    //
    // Reset state + pressure data ...
    //
    // Unscale the sync projection variables for rz.
    //
    rescaleVar(SYNC_PROJ,&sig,1,Vsync,c_lev);
    //
    // Add projected Vsync to new velocity at this level & add phi to pressure.
    //
    AddPhi(pres, phi);
    UpdateArg1(vel, dt_crse, *Vsync, BL_SPACEDIM, grids, 1);

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
        {
            std::cout << "Projection:syncProject(): c_lev: "
                      << c_lev
                      << ", time: " << run_time << '\n';
        }
    }
}

//
//  MULTI-LEVEL SYNC_PROJECT
//

void
Projection::MLsyncProject (int             c_lev,
                           MultiFab&       pres_crse,
                           MultiFab&       vel_crse,
                           MultiFab&       cc_rhs_crse,
                           MultiFab&       pres_fine,
                           MultiFab&       vel_fine,
                           MultiFab&       cc_rhs_fine,
                           MultiFab&       rho_crse,
                           MultiFab&       rho_fine,
                           MultiFab*       Vsync,
                           MultiFab&       V_corr,
                           MultiFab&       phi_fine,
                           SyncRegister*   rhs_sync_reg,
                           SyncRegister*   crsr_sync_reg,
                           Real            dt_crse, 
                           IntVect&        ratio,
                           int             crse_iteration,
                           int             crse_dt_ratio,
                           const Geometry& crse_geom,
                           bool		   pressure_time_is_interval,
                           bool first_crse_step_after_initial_iters,
                           Real             cur_crse_pres_time,
                           Real            prev_crse_pres_time,
                           Real             cur_fine_pres_time,
                           Real            prev_fine_pres_time)
{
    BL_PROFILE("Projection::MLsyncProject()");

    if (verbose && ParallelDescriptor::IOProcessor()) 
        std::cout << "SyncProject: levels = " << c_lev << ", " << c_lev+1 << '\n';

    if (verbose && benchmarking) ParallelDescriptor::Barrier();

    const Real strt_time = ParallelDescriptor::second();
    
    //
    // Set up memory.
    //
    MultiFab *phi[maxlev] = {0};

    const BoxArray& grids      = LevelData[c_lev].boxArray();
    const BoxArray& fine_grids = LevelData[c_lev+1].boxArray();
    const BoxArray& Pgrids_crse = pres_crse.boxArray();
    const BoxArray& Pgrids_fine = pres_fine.boxArray();

    phi[c_lev] = new MultiFab(Pgrids_crse,1,1);
    phi[c_lev]->setVal(0);
    
    phi[c_lev+1] = new MultiFab(Pgrids_fine,1,1);
    phi[c_lev+1]->setVal(0);

    //
    // Set up crse RHS
    //
    PArray<MultiFab> rhnd(1, PArrayManage);
    rhnd.set(0,new MultiFab(Pgrids_crse,1,1));
    rhs_sync_reg->InitRHS(rhnd[0],crse_geom,phys_bc);

    Box P_finedomain(BoxLib::surroundingNodes(crse_geom.Domain()));
    P_finedomain.refine(ratio);
    if (Pgrids_fine[0] == P_finedomain)
        rhnd[c_lev].setVal(0);
    //
    // Do necessary scaling
    //
    scaleVar(SYNC_PROJ,&rho_crse, 0, Vsync,   c_lev  );
    scaleVar(SYNC_PROJ,&rho_fine, 0, &V_corr, c_lev+1);

    if (Geometry::IsRZ()) {
       radMult(c_lev  ,cc_rhs_crse,0);
       radMult(c_lev+1,cc_rhs_fine,0);
    }

    MultiFab* vel[maxlev] = {0};
    MultiFab* sig[maxlev] = {0};
    MultiFab* rhs[maxlev] = {0};

    vel[c_lev  ] = Vsync;
    vel[c_lev+1] = &V_corr;
    sig[c_lev  ] = &rho_crse;
    sig[c_lev+1] = &rho_fine;
    rhs[c_lev  ] = &cc_rhs_crse; 
    rhs[c_lev+1] = &cc_rhs_fine;

    NavierStokes* crse_lev = dynamic_cast<NavierStokes*>(&LevelData[c_lev  ]);
    NavierStokes* fine_lev = dynamic_cast<NavierStokes*>(&LevelData[c_lev+1]);
    const Geometry& fine_geom = parent->Geom(c_lev+1);

    {
      MultiFab v_crse(grids, 1, 1);
      MultiFab v_fine(fine_grids, 1, 1);
      for (int n = 0; n < BL_SPACEDIM; n++) {
    	MultiFab::Copy(v_crse, *vel[c_lev  ], n, 0, 1, 1);
    	MultiFab::Copy(v_fine, *vel[c_lev+1], n, 0, 1, 1);

    	// restrict_level(v_crse, v_fine, ratio);
        BoxLib::average_down(v_fine,v_crse,fine_geom,crse_geom,
                             0, v_crse.nComp(), ratio);


    	MultiFab::Copy(*vel[c_lev  ], v_crse, 0, n, 1, 1);
      }

      // restrict_level(*sig[c_lev], *sig[c_lev+1], ratio);
      BoxLib::average_down(*sig[c_lev+1],*sig[c_lev],fine_geom,crse_geom,
                           0, sig[c_lev]->nComp(), ratio);
    }

    MultiFab*   sync_resid_crse = 0;
    MultiFab*   sync_resid_fine = 0;

    if (c_lev > 0 && (!proj_2 || crse_iteration == crse_dt_ratio))
      //    if (c_lev > 0)
    {
        int ngrow = parent->MaxRefRatio(c_lev-1) - 1;
        sync_resid_fine = new MultiFab(Pgrids_crse,1,ngrow);
    }

    doNodalProjection(c_lev, 2, vel, phi, sig, rhs, rhnd, sync_tol, proj_abs_tol,
		      sync_resid_crse, sync_resid_fine);

    //
    // If this sync project is not at levels 0-1 then we need to account for
    // the changes made here in the level c_lev velocity in the sync registers
    // going into the level (c_lev-1) sync project.  Note that this must be
    // done before rho_half is scaled back.
    //
    if (c_lev > 0 && (!proj_2 || crse_iteration == crse_dt_ratio))
    {
        const Real invrat         = 1.0/(double)crse_dt_ratio;
        const Geometry& crsr_geom = parent->Geom(c_lev-1);
        BoxArray sync_boxes       = pres_fine.boxArray();
        sync_boxes.coarsen(ratio);
        crsr_sync_reg->CompAdd(sync_resid_fine,crse_geom,crsr_geom,
                               phys_bc,sync_boxes,invrat);
    }
    delete sync_resid_fine;
    //
    // Do necessary un-scaling.
    //
    rescaleVar(SYNC_PROJ,&rho_crse, 0, Vsync,   c_lev  );
    rescaleVar(SYNC_PROJ,&rho_fine, 0, &V_corr, c_lev+1);

    for (MFIter phimfi(*phi[c_lev+1]); phimfi.isValid(); ++phimfi) 
    {
        phi_fine[phimfi].copy((*phi[c_lev+1])[phimfi],0,0,1);
    }
    //
    // Add phi to pressure.
    //
    AddPhi(pres_crse, *phi[c_lev]);

    if (pressure_time_is_interval) 
    {
        //
        // Only update the most recent pressure.
        //
        AddPhi(pres_fine, *phi[c_lev+1]);
    }
    else 
    {
        MultiFab& pres_fine_old = LevelData[c_lev+1].get_old_data(Press_Type);
 
        if (first_crse_step_after_initial_iters)
        {
            Real time_since_zero =  cur_crse_pres_time - prev_crse_pres_time;
            Real dt_to_prev_time = prev_fine_pres_time - prev_crse_pres_time;
            Real dt_to_cur_time  =  cur_fine_pres_time - prev_crse_pres_time;

            Real cur_mult_factor = dt_to_cur_time / time_since_zero;
            (*phi[c_lev+1]).mult(cur_mult_factor);
            AddPhi(pres_fine, *phi[c_lev+1]);

            Real prev_mult_factor = dt_to_prev_time / dt_to_cur_time;
            (*phi[c_lev+1]).mult(prev_mult_factor);
            AddPhi(pres_fine_old, *phi[c_lev+1]);
        }
        else 
        {
            AddPhi(pres_fine    , *phi[c_lev+1]);
            AddPhi(pres_fine_old, *phi[c_lev+1]);
        }
    }
    //
    // Add projected vel to new velocity.
    //
    UpdateArg1(vel_crse, dt_crse, *Vsync, BL_SPACEDIM, grids,      1);
    UpdateArg1(vel_fine, dt_crse, V_corr, BL_SPACEDIM, fine_grids, 1);

    for (int lev=c_lev; lev<c_lev+2; lev++) {
      delete phi[lev];
    }

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
        {
            std::cout << "Projection::MLsyncProject(): levels = "
                      << c_lev << ", " << c_lev+1
                      << ", time: " << run_time << '\n';
        }
    }
}

//
// The initial velocity projection in post_init.
// this function ensures that the velocities are nondivergent
//

void
Projection::initialVelocityProject (int  c_lev,
                                    Real cur_divu_time, 
                                    int  have_divu)
{
    int lev;
    int f_lev = parent->finestLevel();

    if (verbose && ParallelDescriptor::IOProcessor()) 
    {
        std::cout << "initialVelocityProject: levels = " << c_lev
                  << "  " << f_lev << '\n';
        if (rho_wgt_vel_proj) 
            std::cout << "RHO WEIGHTED INITIAL VELOCITY PROJECTION\n";
        else 
            std::cout << "CONSTANT DENSITY INITIAL VELOCITY PROJECTION\n";
    }

    if (verbose && benchmarking) ParallelDescriptor::Barrier();

    const Real strt_time = ParallelDescriptor::second();

    MultiFab* vel[maxlev] = {0};
    MultiFab* phi[maxlev] = {0};
    MultiFab* sig[maxlev] = {0};

    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        LevelData[lev].get_old_data(Press_Type).setVal(0);
    }

    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        vel[lev] = &LevelData[lev].get_new_data(State_Type);
        phi[lev] = &LevelData[lev].get_old_data(Press_Type);

        const int       nghost = 1;
        const BoxArray& grids  = LevelData[lev].boxArray();
        sig[lev]               = new MultiFab(grids,1,nghost);

        if (rho_wgt_vel_proj) 
        {
            LevelData[lev].get_new_data(State_Type).setBndry(BogusValue,Density,1);

	    AmrLevel& amr_level = parent->getLevel(lev);
	    
	    MultiFab& S_new = amr_level.get_new_data(State_Type);
	    
            Real curr_time = amr_level.get_state_data(State_Type).curTime();
	    
            for (MFIter mfi(S_new); mfi.isValid(); ++mfi) {
	      amr_level.setPhysBoundaryValues(S_new[mfi],State_Type,curr_time,
					      Density,Density,1);
            }

            MultiFab::Copy(*sig[lev],
                           LevelData[lev].get_new_data(State_Type),
                           Density,
                           0,
                           1,
                           nghost);
        }
        else 
        {
            sig[lev]->setVal(1,nghost);
        }
    }

    MultiFab* rhs_cc[maxlev] = {0};
    const int nghost = 1; 

    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        vel[lev]->setBndry(BogusValue,Xvel,BL_SPACEDIM);
        //
        // Set the physical boundary values.
        //
        AmrLevel& amr_level = parent->getLevel(lev);

        MultiFab& S_new = amr_level.get_new_data(State_Type);

        Real curr_time = amr_level.get_state_data(State_Type).curTime();

        for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
        {
            amr_level.setPhysBoundaryValues(S_new[mfi],State_Type,curr_time,Xvel,Xvel,BL_SPACEDIM);
        }

        if (have_divu) 
        {
            int Divu_Type, Divu;
            if (!LevelData[lev].isStateVariable("divu", Divu_Type, Divu)) 
                BoxLib::Error("Projection::initialVelocityProject(): Divu not found");
            //
            // Make sure ghost cells are properly filled.
            //
            MultiFab& divu_new = amr_level.get_new_data(Divu_Type);
            divu_new.FillBoundary();

            Real curr_time = amr_level.get_state_data(Divu_Type).curTime();

            for (MFIter mfi(divu_new); mfi.isValid(); ++mfi)
            {
                amr_level.setPhysBoundaryValues(divu_new[mfi],Divu_Type,curr_time,0,0,1);
            }
	    
            const BoxArray& grids     = amr_level.boxArray();
            rhs_cc[lev]  = new MultiFab(grids,1,nghost);
            MultiFab* rhslev = rhs_cc[lev];
            put_divu_in_cc_rhs(*rhslev,lev,cur_divu_time);
        }
    }

    if (OutFlowBC::HasOutFlowBC(phys_bc) && do_outflow_bcs && have_divu)
       set_outflow_bcs(INITIAL_VEL,phi,vel,rhs_cc,sig,c_lev,f_lev,have_divu);

     //
     // Scale the projection variables.
     //
    for (lev = c_lev; lev <= f_lev; lev++) 
       scaleVar(INITIAL_VEL,sig[lev],1,vel[lev],lev);

    //
    // Project
    //
    if (!have_divu)
    {
        MultiFab* rhs[maxlev] = {0};
	PArray<MultiFab> rhnd;
	doNodalProjection(c_lev, f_lev+1, vel, phi, sig, rhs, rhnd, 
			  proj_tol, proj_abs_tol);
    } 
    else 
    {
        PArray<MultiFab> rhs_real(f_lev+1,PArrayManage);
        for (lev = c_lev; lev <= f_lev; lev++) 
        {
            MultiFab* rhslev = rhs_cc[lev];
            if (Geometry::IsRZ()) radMult(lev,*rhslev,0); 
            rhs_cc[lev]->mult(-1.0,0,1,nghost);
            rhs_real.set(lev, rhs_cc[lev]);
        }

	PArray<MultiFab> rhnd;
	doNodalProjection(c_lev, f_lev+1, vel, phi, sig, rhs_cc, rhnd,
			  proj_tol, proj_abs_tol);

    }

    //
    // Unscale initial projection variables.
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
        rescaleVar(INITIAL_VEL,sig[lev],1,vel[lev],lev);

    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        LevelData[lev].get_old_data(Press_Type).setVal(0);
        LevelData[lev].get_new_data(Press_Type).setVal(0);
    }

    for (lev = c_lev; lev <= f_lev; lev++) {
      delete sig[lev];
    }

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - strt_time;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if ( ParallelDescriptor::IOProcessor())
            std::cout << "Projection::initialVelocityProject(): time: " << run_time << '\n';
    }
}

void
Projection::initialPressureProject (int  c_lev)
                                    
{
    int lev;
    int f_lev = parent->finestLevel();
    if (verbose && ParallelDescriptor::IOProcessor()) 
        std::cout << "initialPressureProject: levels = " << c_lev
                  << "  " << f_lev << '\n';

    MultiFab* vel[maxlev] = {0};
    MultiFab* phi[maxlev] = {0};
    MultiFab* sig[maxlev] = {0};

    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        vel[lev] = &LevelData[lev].get_new_data(State_Type);
        phi[lev] = &LevelData[lev].get_old_data(Press_Type);

        const int       nghost = 1;
        const BoxArray& grids  = LevelData[lev].boxArray();
        sig[lev]               = new MultiFab(grids,1,nghost);

        LevelData[lev].get_new_data(State_Type).setBndry(BogusValue,Density,1);

        AmrLevel& amr_level = parent->getLevel(lev);

        MultiFab& S_new = amr_level.get_new_data(State_Type);

        S_new.setBndry(BogusValue,Density,1);

        Real curr_time = amr_level.get_state_data(State_Type).curTime();

        for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
        {
            amr_level.setPhysBoundaryValues(S_new[mfi],State_Type,curr_time,Density,Density,1);
        }

        const Geometry& geom = parent->Geom(lev);

        MultiFab::Copy(*sig[lev],
                       LevelData[lev].get_new_data(State_Type),
                       Density,
                       0,
                       1,
                       nghost);

        EnforcePeriodicity(*sig[lev],1,grids,geom);
    }

    //
    // Set up outflow bcs.
    //
    NavierStokes* ns = dynamic_cast<NavierStokes*>(&LevelData[c_lev]);
    Real gravity = ns->getGravity();

    if (OutFlowBC::HasOutFlowBC(phys_bc) && do_outflow_bcs)
    {
        int have_divu_dummy = 0;
        MultiFab* Divu_ML[maxlev] = {0};

        set_outflow_bcs(INITIAL_PRESS,phi,vel,Divu_ML,sig,
                        c_lev,f_lev,have_divu_dummy);
    }

    //
    // Scale the projection variables.
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
        scaleVar(INITIAL_PRESS,sig[lev],1,vel[lev],lev);

    for (lev = c_lev; lev <= f_lev; lev++) {
      BoxArray grids = vel[lev]->boxArray();

      vel[lev] = new MultiFab(grids, BL_SPACEDIM, 1);
      vel[lev]->setVal(0.0    , 0            , BL_SPACEDIM-1, 1);
      vel[lev]->setVal(gravity, BL_SPACEDIM-1, 1            , 1);
    }

    //
    // Project
    //
    MultiFab* rhs[maxlev] = {0};
    PArray<MultiFab> rhnd;
    doNodalProjection(c_lev, f_lev+1, vel, phi, sig, rhs, rhnd,
		      proj_tol, proj_abs_tol);

    //
    // Unscale initial projection variables.
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
        rescaleVar(INITIAL_PRESS,sig[lev],1,vel[lev],lev);

    //
    // Copy "old" pressure just computed into "new" pressure as well.
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
        MultiFab::Copy(LevelData[lev].get_new_data(Press_Type),
                       LevelData[lev].get_old_data(Press_Type),
                       0, 0, 1, 0);

    for (lev = c_lev; lev <= f_lev; lev++) {
      delete vel[lev];
      delete sig[lev];
    }
}

//
// The velocity projection in post_init, which computes the initial
// pressure used in the timestepping.
//

void
Projection::initialSyncProject (int       c_lev,
                                MultiFab* sig[],
                                Real      dt, 
                                Real      strt_time,
                                int       have_divu)
{
    int lev;
    int f_lev = parent->finestLevel();

    if (verbose && ParallelDescriptor::IOProcessor()) 
        std::cout << "initialSyncProject: levels = " << c_lev << "  " << f_lev << '\n';

    if (verbose && benchmarking) ParallelDescriptor::Barrier();

    const Real stime = ParallelDescriptor::second();
    //
    // Gather data.
    //
    MultiFab* vel[maxlev] = {0};
    MultiFab* phi[maxlev] = {0};
    MultiFab* rhs[maxlev] = {0};

    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        vel[lev] = &LevelData[lev].get_new_data(State_Type);
        phi[lev] = &LevelData[lev].get_old_data(Press_Type);
    }
  
    const Real dt_inv = 1./dt;

    if (have_divu) 
    {
        //
        // Set up rhs for manual project.
        //
        for (lev = c_lev; lev <= f_lev; lev++) 
        {
            AmrLevel& amr_level = parent->getLevel(lev);

            int Divu_Type, Divu;
            if (!LevelData[c_lev].isStateVariable("divu", Divu_Type, Divu)) 
                BoxLib::Error("Projection::initialSyncProject(): Divu not found");
            //
            // Make sure ghost cells are properly filled.
            //
            MultiFab& divu_new = amr_level.get_new_data(Divu_Type);
            MultiFab& divu_old = amr_level.get_old_data(Divu_Type);
            divu_new.FillBoundary();
            divu_old.FillBoundary();

            Real prev_time = amr_level.get_state_data(Divu_Type).prevTime();
            Real curr_time = amr_level.get_state_data(Divu_Type).curTime();

            for (MFIter mfi(divu_new); mfi.isValid(); ++mfi)
            {
                amr_level.setPhysBoundaryValues(divu_old[mfi],Divu_Type,prev_time,0,0,1);
                amr_level.setPhysBoundaryValues(divu_new[mfi],Divu_Type,curr_time,0,0,1);
            }

            const int nghost = 1;
            rhs[lev] = new MultiFab(amr_level.boxArray(),1,nghost);
            MultiFab* rhslev = rhs[lev];
            rhslev->setVal(0);

            NavierStokes* ns = dynamic_cast<NavierStokes*>(&parent->getLevel(lev));

            BL_ASSERT(!(ns == 0));

            MultiFab* divu = ns->getDivCond(nghost,strt_time);
            MultiFab* dsdt = ns->getDivCond(nghost,strt_time+dt);

            for (MFIter mfi(*rhslev); mfi.isValid(); ++mfi)
            {
                FArrayBox& dsdtfab = (*dsdt)[mfi];
                dsdtfab.minus((*divu)[mfi]);
                dsdtfab.mult(dt_inv);
                (*rhslev)[mfi].copy(dsdtfab);
            }

            delete divu;
            delete dsdt;
        }
    }

    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        MultiFab& P_old = LevelData[lev].get_old_data(Press_Type);
        P_old.setVal(0);
    }
    //
    // Set velocity bndry values to bogus values.
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        vel[lev]->setBndry(BogusValue,Xvel,BL_SPACEDIM);
        MultiFab &u_o = LevelData[lev].get_old_data(State_Type);
        u_o.setBndry(BogusValue,Xvel,BL_SPACEDIM);
        sig[lev]->setBndry(BogusValue);
    }
    //
    // Convert velocities to accelerations (we always do this for the
    //  projections in these initial iterations).
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        MultiFab& S_old = LevelData[lev].get_old_data(State_Type);
        MultiFab& S_new = LevelData[lev].get_new_data(State_Type);

        Real prev_time = LevelData[lev].get_state_data(State_Type).prevTime();
        Real curr_time = LevelData[lev].get_state_data(State_Type).curTime();

        for (MFIter mfi(S_new); mfi.isValid(); ++mfi)
        {
            LevelData[lev].setPhysBoundaryValues(S_old[mfi],State_Type,prev_time,Xvel,Xvel,BL_SPACEDIM);
            LevelData[lev].setPhysBoundaryValues(S_new[mfi],State_Type,curr_time,Xvel,Xvel,BL_SPACEDIM);
        }

        MultiFab& u_o = LevelData[lev].get_old_data(State_Type);
        ConvertUnew(*vel[lev], u_o, dt, LevelData[lev].boxArray());
    }

    if (OutFlowBC::HasOutFlowBC(phys_bc) && have_divu && do_outflow_bcs) 
        set_outflow_bcs(INITIAL_SYNC,phi,vel,rhs,sig,
                        c_lev,f_lev,have_divu);

    //
    // Scale initial sync projection variables.
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
    {
        scaleVar(INITIAL_SYNC,sig[lev],1,vel[lev],lev);

        if (have_divu && Geometry::IsRZ()) 
          radMult(lev,*(rhs[lev]),0);    
    }

    for (lev = f_lev; lev >= c_lev+1; lev--) {
      const BoxArray& crse_grids = vel[lev-1]->boxArray();
      const BoxArray& fine_grids = vel[lev  ]->boxArray();

      MultiFab v_crse(crse_grids, BL_SPACEDIM, 1);
      MultiFab v_fine(fine_grids, BL_SPACEDIM, 1);

      NavierStokes* crse_lev = dynamic_cast<NavierStokes*>(&LevelData[lev-1]);

      const Geometry& fine_geom = parent->Geom(lev  );
      const Geometry& crse_geom = parent->Geom(lev-1);

      MultiFab::Copy(v_crse, *vel[lev-1], 0, 0, BL_SPACEDIM, 1);
      MultiFab::Copy(v_fine, *vel[lev  ], 0, 0, BL_SPACEDIM, 1);

      // restrict_level(v_crse, v_fine, parent->refRatio(lev-1));
      BoxLib::average_down(v_fine,v_crse,fine_geom,crse_geom,
                           0, v_crse.nComp(), parent->refRatio(lev-1));
	
      MultiFab::Copy(*vel[lev-1], v_crse, 0, 0, BL_SPACEDIM, 1);
    }

    //
    // Project.
    //
    if (!have_divu) 
    {
        //
        // Zero divu only or debugging.
        //
        PArray<MultiFab> rhnd;
	doNodalProjection(c_lev, f_lev+1, vel, phi, sig, rhs, rhnd,
			  proj_tol, proj_abs_tol);
    } 
    else 
    {
        //
        // General divu.
        //
        //
        // This PArray is managed so it'll remove rhs on exiting scope.
        //
        PArray<MultiFab> rhs_real(f_lev+1,PArrayManage);
        for (lev = c_lev; lev <= f_lev; lev++) 
        {
            rhs[lev]->mult(-1.0,0,1);
            rhs_real.set(lev, rhs[lev]);
        }

        PArray<MultiFab> rhnd;
	doNodalProjection(c_lev, f_lev+1, vel, phi, sig, rhs, rhnd,
			  proj_tol, proj_abs_tol);
    }

    //
    // Unscale initial sync projection variables.
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
        rescaleVar(INITIAL_SYNC,sig[lev],1,vel[lev],lev);
    //
    // Add correction at coarse and fine levels.
    //
    for (lev = c_lev; lev <= f_lev; lev++) 
        incrPress(lev, 1.0);

    if (verbose)
    {
        const int IOProc   = ParallelDescriptor::IOProcessorNumber();
        Real      run_time = ParallelDescriptor::second() - stime;

        ParallelDescriptor::ReduceRealMax(run_time,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "Projection::initialSyncProject(): time: " << run_time << '\n';
    }
}

//
// Put S in the rhs of the projector--cell based version.
//

void
Projection::put_divu_in_cc_rhs (MultiFab&       rhs,
                                int             level,
                                Real            time)
{
    rhs.setVal(0);

    NavierStokes* ns = dynamic_cast<NavierStokes*>(&parent->getLevel(level));

    BL_ASSERT(!(ns == 0));

    MultiFab* divu = ns->getDivCond(1,time);

    for (MFIter mfi(rhs); mfi.isValid(); ++mfi)
    {
        rhs[mfi].copy((*divu)[mfi]);
    }

    delete divu;
}

void
Projection::EnforcePeriodicity (MultiFab&       psi,
                                int             nvar,
                                const BoxArray& /*grids*/,
                                const Geometry& geom)
{
    BL_ASSERT(nvar <= psi.nComp());

    geom.FillPeriodicBoundary(psi,0,nvar);
}

//
// Convert U from an Accl-like quantity to a velocity: Unew = Uold + alpha*Unew
//

void
Projection::UnConvertUnew (MultiFab&       Uold,
                           Real            alpha,
                           MultiFab&       Unew, 
                           const BoxArray& grids)
{
    for (MFIter Uoldmfi(Uold); Uoldmfi.isValid(); ++Uoldmfi) 
    {
        BL_ASSERT(grids[Uoldmfi.index()] == Uoldmfi.validbox());

        UnConvertUnew(Uold[Uoldmfi],alpha,Unew[Uoldmfi],Uoldmfi.validbox());
    }
}

//
// Convert U from an Accleration like quantity to a velocity
// Unew = Uold + alpha*Unew.
//

void
Projection::UnConvertUnew (FArrayBox& Uold,
                           Real       alpha,
                           FArrayBox& Unew,
                           const Box& grd)
{
    BL_ASSERT(Unew.nComp() >= BL_SPACEDIM);
    BL_ASSERT(Uold.nComp() >= BL_SPACEDIM);
    BL_ASSERT(Unew.contains(grd) == true);
    BL_ASSERT(Uold.contains(grd) == true);
    
    const int*  lo    = grd.loVect();
    const int*  hi    = grd.hiVect();
    const int*  uo_lo = Uold.loVect(); 
    const int*  uo_hi = Uold.hiVect(); 
    const Real* uold  = Uold.dataPtr(0);
    const int*  un_lo = Unew.loVect(); 
    const int*  un_hi = Unew.hiVect(); 
    const Real* unew  = Unew.dataPtr(0);
    
    FORT_ACCEL_TO_VEL(lo, hi,
                      uold, ARLIM(uo_lo), ARLIM(uo_hi),
                      &alpha,
                      unew, ARLIM(un_lo), ARLIM(un_hi));
}

//
// Convert U to an Accleration like quantity: Unew = (Unew - Uold)/alpha
//

void
Projection::ConvertUnew (MultiFab&       Unew,
                         MultiFab&       Uold,
                         Real            alpha,
                         const BoxArray& grids)
{
    for (MFIter Uoldmfi(Uold); Uoldmfi.isValid(); ++Uoldmfi) 
    {
        BL_ASSERT(grids[Uoldmfi.index()] == Uoldmfi.validbox());

        ConvertUnew(Unew[Uoldmfi],Uold[Uoldmfi],alpha,Uoldmfi.validbox());
    }
}

//
// Convert U to an Accleration like quantity: Unew = (Unew - Uold)/alpha
//

void
Projection::ConvertUnew( FArrayBox &Unew, FArrayBox &Uold, Real alpha,
                              const Box &grd )
{
    BL_ASSERT(Unew.nComp() >= BL_SPACEDIM);
    BL_ASSERT(Uold.nComp() >= BL_SPACEDIM);
    BL_ASSERT(Unew.contains(grd) == true);
    BL_ASSERT(Uold.contains(grd) == true);
    
    const int*  lo    = grd.loVect();
    const int*  hi    = grd.hiVect();
    const int*  uo_lo = Uold.loVect(); 
    const int*  uo_hi = Uold.hiVect(); 
    const Real* uold  = Uold.dataPtr(0);
    const int*  un_lo = Unew.loVect(); 
    const int*  un_hi = Unew.hiVect(); 
    const Real* unew  = Unew.dataPtr(0);
                    
    FORT_VEL_TO_ACCEL(lo, hi, 
                      unew, ARLIM(un_lo), ARLIM(un_hi),
                      uold, ARLIM(uo_lo), ARLIM(uo_hi), &alpha );
}

//
// Update a quantity U using the formula: Unew = Unew + alpha*Uold
//

void
Projection::UpdateArg1 (MultiFab&       Unew,
                        Real            alpha,
                        MultiFab&       Uold,
                        int             nvar,
                        const BoxArray& grids,
                        int             ngrow)
{
    for (MFIter Uoldmfi(Uold); Uoldmfi.isValid(); ++Uoldmfi) 
    {
        BL_ASSERT(grids[Uoldmfi.index()] == Uoldmfi.validbox());

        UpdateArg1(Unew[Uoldmfi],alpha,Uold[Uoldmfi],nvar,Uoldmfi.validbox(),ngrow);
    }
}

//
// Update a quantity U using the formula
// currently only the velocity, but will do the pressure as well.
// Unew = Unew + alpha*Uold
//

void
Projection::UpdateArg1 (FArrayBox& Unew,
                        Real       alpha,
                        FArrayBox& Uold,
                        int        nvar,
                        const Box& grd,
                        int        ngrow)
{
    BL_ASSERT(nvar <= Uold.nComp());
    BL_ASSERT(nvar <= Unew.nComp());

    Box        b  = BoxLib::grow(grd,ngrow);
    const Box& bb = Unew.box();

    if (bb.ixType() == IndexType::TheNodeType())
        b.surroundingNodes();

    BL_ASSERT(Uold.contains(b) == true);
    BL_ASSERT(Unew.contains(b) == true);

    const int*  lo    = b.loVect();
    const int*  hi    = b.hiVect();
    const int*  uo_lo = Uold.loVect(); 
    const int*  uo_hi = Uold.hiVect(); 
    const Real* uold  = Uold.dataPtr(0);
    const int*  un_lo = Unew.loVect(); 
    const int*  un_hi = Unew.hiVect(); 
    const Real* unew  = Unew.dataPtr(0);
                    
    FORT_PROJ_UPDATE(lo,hi,&nvar,&ngrow,
                     unew, ARLIM(un_lo), ARLIM(un_hi),
                     &alpha,
                     uold, ARLIM(uo_lo), ARLIM(uo_hi) );
}

//
// Add phi to P.
//

void
Projection::AddPhi (MultiFab&        p,
                    MultiFab&       phi)
{
    for (MFIter pmfi(p); pmfi.isValid(); ++pmfi) 
    {
        p[pmfi].plus(phi[pmfi]);
    }
}

//
// Convert phi into p^n+1/2.
//

void
Projection::incrPress (int  level,
                       Real dt)
{
    MultiFab& P_old = LevelData[level].get_old_data(Press_Type);
    MultiFab& P_new = LevelData[level].get_new_data(Press_Type);

    const BoxArray& grids = LevelData[level].boxArray();

    for (MFIter P_newmfi(P_new); P_newmfi.isValid(); ++P_newmfi)
    {
        const int i = P_newmfi.index();

        UpdateArg1(P_new[P_newmfi],1.0/dt,P_old[P_newmfi],1,grids[i],1);

        P_old[P_newmfi].setVal(BogusValue);
    }
}

//
// This function scales variables at the start of a projection.
//

void
Projection::scaleVar (int             which_call,
                      MultiFab*       sig,
                      int             sig_nghosts,
                      MultiFab*       vel,
                      int             level)
{
    BL_ASSERT((which_call == INITIAL_VEL  ) || 
              (which_call == INITIAL_PRESS) || 
              (which_call == INITIAL_SYNC ) ||
              (which_call == LEVEL_PROJ   ) ||
              (which_call == SYNC_PROJ    ) );

    if (sig != 0)
        BL_ASSERT(sig->nComp() == 1);
    if (vel != 0)
        BL_ASSERT(vel->nComp() >= BL_SPACEDIM);

    //
    // Convert sigma from rho to anel_coeff/rho if not INITIAL_PRESS.
    // nghosts info needed to avoid divide by zero.
    //
    if (sig != 0) {
      sig->invert(1.0,sig_nghosts);
      if (which_call  != INITIAL_PRESS &&
          anel_coeff[level] != 0) AnelCoeffMult(level,*sig,0);
    }

    //
    // Scale by radius for RZ.
    //
    if (Geometry::IsRZ()) 
    {
        if (sig != 0)
            radMult(level,*sig,0);
        if (vel != 0)
            for (int n = 0; n < BL_SPACEDIM; n++) 
                radMult(level,*vel,n);
    }

    //
    // Scale velocity by anel_coeff if it exists
    //
    if (vel != 0 && anel_coeff[level] != 0)
      for (int n = 0; n < BL_SPACEDIM; n++) 
        AnelCoeffMult(level,*vel,n);
}

//
// This function rescales variables at the end of a projection.
//

void
Projection::rescaleVar (int             which_call,
                        MultiFab*       sig,
                        int             sig_nghosts,
                        MultiFab*       vel,
                        int             level)
{
    BL_ASSERT((which_call == INITIAL_VEL  ) || 
              (which_call == INITIAL_PRESS) || 
              (which_call == INITIAL_SYNC ) ||
              (which_call == LEVEL_PROJ   ) ||
              (which_call == SYNC_PROJ    ) );

    if (sig != 0)
        BL_ASSERT(sig->nComp() == 1);
    if (vel != 0)
        BL_ASSERT(vel->nComp() >= BL_SPACEDIM);

    if (which_call  != INITIAL_PRESS && sig != 0 &&
        anel_coeff[level] != 0) AnelCoeffDiv(level,*sig,0);
    //
    // Divide by radius to rescale for RZ coordinates.
    //
    if (Geometry::IsRZ()) 
    {
        if (sig != 0)
            radDiv(level,*sig,0);
        if (vel != 0)
            for (int n = 0; n < BL_SPACEDIM; n++)
                radDiv(level,*vel,n);
    }
    if (vel != 0 && anel_coeff[level] != 0) 
      for (int n = 0; n < BL_SPACEDIM; n++)
        AnelCoeffDiv(level,*vel,n);
    //
    // Convert sigma from 1/rho to rho
    // NOTE: this must come after division by r to be correct,
    // nghosts info needed to avoid divide by zero.
    //
    if (sig != 0)
        sig->invert(1.0,sig_nghosts);
}

//
// Multiply by a radius for r-z coordinates.
//

void
Projection::radMult (int       level,
                     MultiFab& mf,
                     int       comp)
{
    BL_ASSERT(radius_grow >= mf.nGrow());
    BL_ASSERT(comp >= 0 && comp < mf.nComp());

    int ngrow = mf.nGrow();

    int nr = radius_grow;

    const Box& domain = parent->Geom(level).Domain();
    const int* domlo  = domain.loVect();
    const int* domhi  = domain.hiVect();

    Real bogus_value = BogusValue;

    for (MFIter mfmfi(mf); mfmfi.isValid(); ++mfmfi) 
    {
        BL_ASSERT(mf.box(mfmfi.index()) == mfmfi.validbox());

        const Box& bx = mfmfi.validbox();
        const int* lo = bx.loVect();
        const int* hi = bx.hiVect();
        Real* dat     = mf[mfmfi].dataPtr(comp);
        Real* rad     = &radius[level][mfmfi.index()][0];

        FORT_RADMPY(dat,ARLIM(lo),ARLIM(hi),domlo,domhi,&ngrow,
                    rad,&nr,&bogus_value);
    }
}

//
// Divide by a radius for r-z coordinates.
//

void
Projection::radDiv (int       level,
                    MultiFab& mf,
                    int       comp)
{
    BL_ASSERT(comp >= 0 && comp < mf.nComp());
    BL_ASSERT(radius_grow >= mf.nGrow());

    int ngrow = mf.nGrow();
    int nr    = radius_grow;

    const Box& domain = parent->Geom(level).Domain();
    const int* domlo  = domain.loVect();
    const int* domhi  = domain.hiVect();

    Real bogus_value = BogusValue;

    for (MFIter mfmfi(mf); mfmfi.isValid(); ++mfmfi) 
    {
        BL_ASSERT(mf.box(mfmfi.index()) == mfmfi.validbox());

        const Box& bx  = mfmfi.validbox();
        const int* lo  = bx.loVect();
        const int* hi  = bx.hiVect();
        Real*      dat = mf[mfmfi].dataPtr(comp);
        Real*      rad = &radius[level][mfmfi.index()][0];

        FORT_RADDIV(dat,ARLIM(lo),ARLIM(hi),domlo,domhi,&ngrow,
                    rad,&nr,&bogus_value);
    }
}

//
// Multiply by anel_coeff if it is defined
//
void
Projection::AnelCoeffMult (int       level,
                           MultiFab& mf,
                           int       comp)
{
    BL_ASSERT(anel_coeff[level] != 0);
    BL_ASSERT(comp >= 0 && comp < mf.nComp());
    int ngrow = mf.nGrow();
    int nr    = 1;

    const Box& domain = parent->Geom(level).Domain();
    const int* domlo  = domain.loVect();
    const int* domhi  = domain.hiVect();

    Real bogus_value = BogusValue;

    int mult = 1;

    for (MFIter mfmfi(mf); mfmfi.isValid(); ++mfmfi) 
    {
        BL_ASSERT(mf.box(mfmfi.index()) == mfmfi.validbox());

        const Box& bx = mfmfi.validbox();
        const int* lo = bx.loVect();
        const int* hi = bx.hiVect();
        Real* dat     = mf[mfmfi].dataPtr(comp);

        FORT_ANELCOEFFMPY(dat,ARLIM(lo),ARLIM(hi),domlo,domhi,&ngrow,
                          anel_coeff[level][mfmfi.index()],&nr,&bogus_value,&mult);
    }
}

//
// Divide by anel_coeff if it is defined
//
void
Projection::AnelCoeffDiv (int       level,
                          MultiFab& mf,
                          int       comp)
{
    BL_ASSERT(comp >= 0 && comp < mf.nComp());
    BL_ASSERT(anel_coeff[level] != 0);
    int ngrow = mf.nGrow();
    int nr    = 1;

    const Box& domain = parent->Geom(level).Domain();
    const int* domlo  = domain.loVect();
    const int* domhi  = domain.hiVect();

    Real bogus_value = BogusValue;

    int mult = 0;

    for (MFIter mfmfi(mf); mfmfi.isValid(); ++mfmfi) 
    {
        BL_ASSERT(mf.box(mfmfi.index()) == mfmfi.validbox());

        const Box& bx = mfmfi.validbox();
        const int* lo = bx.loVect();
        const int* hi = bx.hiVect();
        Real* dat     = mf[mfmfi].dataPtr(comp);

        FORT_ANELCOEFFMPY(dat,ARLIM(lo),ARLIM(hi),domlo,domhi,&ngrow,
                          anel_coeff[level][mfmfi.index()],&nr,&bogus_value,&mult);
    }
}

//
// This projects the initial vorticity field (stored in pressure)
// to define an initial velocity field.
//
void
Projection::initialVorticityProject (int c_lev)
{
#if (BL_SPACEDIM == 2)
  int f_lev = parent->finestLevel();

    if (verbose && ParallelDescriptor::IOProcessor())
    {
        std::cout << "initialVorticityProject: levels = "
                  << c_lev
                  << "  "
                  << f_lev << std::endl;
    }
    //
    // Set up projector bndry just for this projection.
    //
    const Geometry& geom = parent->Geom(0);

    MultiFab* p_real[maxlev] = {0};
    MultiFab* s_real[maxlev] = {0};

    for (int lev = c_lev; lev <= f_lev; lev++)
    {
        MultiFab& P_new  = LevelData[lev].get_new_data(Press_Type);
        const int nghost = 1;
        s_real[lev] = new MultiFab(LevelData[lev].boxArray(),1,nghost);
        s_real[lev]->setVal(1,nghost);
        p_real[lev] = new MultiFab(P_new.boxArray(),1,nghost);
        p_real[lev]->setVal(0,nghost);
    }
    //
    // Set up outflow bcs.
    //
    MultiFab* u_real[maxlev] = {0};
    MultiFab* rhs_cc[maxlev] = {0};
    PArray<MultiFab> rhnd(maxlev, PArrayManage);

    for (int lev = c_lev; lev <= f_lev; lev++)
    {
        const BoxArray& full_mesh = parent->getLevel(lev).boxArray();

        u_real[lev] = new MultiFab(full_mesh, BL_SPACEDIM, 1);
        u_real[lev]->setVal(0);
        //
        // The vorticity is stored in the new pressure variable for now.
        //
        MultiFab& P_new = LevelData[lev].get_new_data(Press_Type);

        rhnd.set(lev, new MultiFab(P_new.boxArray(), 1, 1));

        for (MFIter mfi(rhnd[lev]); mfi.isValid(); ++mfi)
        {
          rhnd[lev][mfi].setVal(0);
          rhnd[lev][mfi].copy(P_new[mfi], 0, 0);
        }
    }

    //
    // Set BC for vorticity solve, save a copy of orig ones
    //
    BCRec phys_bc_save(phys_bc->lo(),phys_bc->hi());
    for (int i=0; i<BL_SPACEDIM; ++i) {
      phys_bc->setLo(i,Outflow);
      phys_bc->setHi(i,Outflow);
      if (geom.isPeriodic(i)) {
        phys_bc->setLo(i,Interior);
        phys_bc->setHi(i,Interior);
      }
    }
    //
    // Project.
    //
    doNodalProjection(c_lev, f_lev+1, u_real, p_real, s_real, rhs_cc, rhnd,
                      proj_tol, proj_abs_tol);

    //
    // Generate velocity field from potential
    //
    const int idx[2] = {1, 0};

    MultiFab* vel[maxlev] = {0};
    for (int lev = c_lev; lev <= f_lev; lev++)
    {
        vel[lev] = &LevelData[lev].get_new_data(State_Type);
        //
        // Note: Here u_real from projection is -grad(phi), but if
        //  phi is the stream function, u=dphi/dy, v=-dphi/dx
        //
        (*u_real[lev]).mult(-1,Yvel,1);

        for (int n = 0; n < BL_SPACEDIM; n++)
        {
            for (MFIter mfi(*vel[lev]); mfi.isValid(); ++mfi)
            {
                const Box& box = mfi.validbox();
                if (add_vort_proj)
                {
                  (*vel[lev])[mfi].plus((*u_real[lev])[mfi],box,Xvel+n,Xvel+idx[n], 1);
                }
                else
                {
                  (*vel[lev])[mfi].copy((*u_real[lev])[mfi],box,Xvel+n,box,Xvel+idx[n], 1);
                }
            }
        }
    }

    //
    // Restore bcs
    //
    for (int i=0; i<BL_SPACEDIM; ++i) {
      phys_bc->setLo(i,phys_bc_save.lo()[i]);
      phys_bc->setHi(i,phys_bc_save.hi()[i]);
    }


#else
    BoxLib::Error("Projection::initialVorticityProject(): not implented yet for 3D");
#endif
}

void 
Projection::putDown (MultiFab**         phi,
                     FArrayBox*         phi_fine_strip,
                     int                c_lev,
                     int                f_lev,
                     const Orientation* outFaces,
                     int                numOutFlowFaces,
                     int                ncStripWidth)
{
    BL_PROFILE("Projection::putDown()");
    //
    // Put down to coarser levels.
    //
    const int nCompPhi = 1; // phi_fine_strip.nComp();
    const int nGrow    = 0; // phi_fine_strip.nGrow();
    IntVect ratio      = IntVect::TheUnitVector();

    for (int lev = f_lev-1; lev >= c_lev; lev--)
    {
        ratio *= parent->refRatio(lev);
        const Box& domainC = parent->Geom(lev).Domain();

        for (int iface = 0; iface < numOutFlowFaces; iface++) 
        {
            Box phiC_strip = 
                BoxLib::surroundingNodes(BoxLib::bdryNode(domainC, outFaces[iface], ncStripWidth));
            phiC_strip.grow(nGrow);
            BoxArray ba(phiC_strip);
            MultiFab phi_crse_strip(ba, nCompPhi, 0);
            phi_crse_strip.setVal(0);

            for (MFIter mfi(phi_crse_strip); mfi.isValid(); ++mfi)
            {
                Box ovlp = BoxLib::coarsen(phi_fine_strip[iface].box(),ratio) & mfi.validbox();

                if (ovlp.ok())
                {
                    FArrayBox& cfab = phi_crse_strip[mfi];
                    FORT_PUTDOWN (BL_TO_FORTRAN(cfab),
                                  BL_TO_FORTRAN(phi_fine_strip[iface]),
                                  ovlp.loVect(), ovlp.hiVect(), ratio.getVect());
                }
            }

            phi[lev]->copy(phi_crse_strip);
        }
    }
}

void
Projection::getStreamFunction (PArray<MultiFab>& phi)
{
  BoxLib::Abort("Projection::getStreamFunction not implemented");
}

//
// Given a nodal pressure P compute the pressure gradient at the
// contained cell centers.

void
Projection::getGradP (FArrayBox& p_fab,
                      FArrayBox& gp,
                      const Box& gpbox_to_fill,
                      const Real* dx)
{
    BL_PROFILE("Projection::getGradP()");
    //
    // Test to see if p_fab contains gpbox_to_fill
    //
    BL_ASSERT(BoxLib::enclosedCells(p_fab.box()).contains(gpbox_to_fill));

    const int*  plo    = p_fab.loVect();
    const int*  phi    = p_fab.hiVect();
    const int*  glo    = gp.box().loVect();
    const int*  ghi    = gp.box().hiVect();
    const int*   lo    = gpbox_to_fill.loVect();
    const int*   hi    = gpbox_to_fill.hiVect();
    const Real* p_dat  = p_fab.dataPtr();
    const Real* gp_dat = gp.dataPtr();

#if (BL_SPACEDIM == 2)
    int is_full = 0;
    FORT_GRADP(p_dat,ARLIM(plo),ARLIM(phi),gp_dat,ARLIM(glo),ARLIM(ghi),lo,hi,dx,
               &is_full);
#elif (BL_SPACEDIM == 3)
    FORT_GRADP(p_dat,ARLIM(plo),ARLIM(phi),gp_dat,ARLIM(glo),ARLIM(ghi),lo,hi,dx);
#endif
}

void
Projection::set_outflow_bcs (int        which_call,
                             MultiFab** phi, 
                             MultiFab** Vel_in,
                             MultiFab** Divu_in,
                             MultiFab** Sig_in,
                             int        c_lev,
                             int        f_lev,
                             int        have_divu)
{
    BL_ASSERT((which_call == INITIAL_VEL  ) || 
              (which_call == INITIAL_PRESS) || 
              (which_call == INITIAL_SYNC ) ||
              (which_call == LEVEL_PROJ   ) );

    if (which_call != LEVEL_PROJ)
      BL_ASSERT(c_lev == 0);

    if ( verbose && ParallelDescriptor::IOProcessor() ) 
	std::cout << "...setting outflow bcs for the nodal projection ... " << '\n';

    bool        hasOutFlow;
    Orientation outFaces[2*BL_SPACEDIM];
    Orientation outFacesAtThisLevel[maxlev][2*BL_SPACEDIM];

    int fine_level[2*BL_SPACEDIM];

    int numOutFlowFacesAtAllLevels;
    int numOutFlowFaces[maxlev];
    OutFlowBC::GetOutFlowFaces(hasOutFlow,outFaces,phys_bc,numOutFlowFacesAtAllLevels);

    //
    // Get 2-wide cc box, state_strip, along interior of top. 
    // Get 1-wide nc box, phi_strip  , along top.
    //
    const int ccStripWidth = 2;

//    const int nCompPhi    = 1;
//    const int srcCompVel  = Xvel;
//    const int srcCompDivu = 0;
//    const int   nCompVel  = BL_SPACEDIM;
//    const int   nCompDivu = 1;

    //
    // Determine the finest level such that the entire outflow face is covered
    // by boxes at this level (skip if doesnt touch, and bomb if only partially
    // covered).
    //
    Box state_strip[maxlev][2*BL_SPACEDIM];

    int icount[maxlev];
    for (int i=0; i < maxlev; i++) icount[i] = 0;

    //
    // This loop is only to define the number of outflow faces at each level.
    //
    Box temp_state_strip;
    for (int iface = 0; iface < numOutFlowFacesAtAllLevels; iface++) 
    {
      const int outDir    = outFaces[iface].coordDir();

      fine_level[iface] = -1;
      for (int lev = f_lev; lev >= c_lev; lev--)
      {
        Box domain = parent->Geom(lev).Domain();

        if (outFaces[iface].faceDir() == Orientation::high)
        {
            temp_state_strip = BoxLib::adjCellHi(domain,outDir,ccStripWidth);
            temp_state_strip.shift(outDir,-ccStripWidth);
        }
        else
        {
            temp_state_strip = BoxLib::adjCellLo(domain,outDir,ccStripWidth);
            temp_state_strip.shift(outDir,ccStripWidth);
        }
        // Grow the box by one tangentially in order to get velocity bc's.
        for (int dir = 0; dir < BL_SPACEDIM; dir++) 
          if (dir != outDir) temp_state_strip.grow(dir,1);

        const BoxArray& Lgrids               = parent->getLevel(lev).boxArray();
        const Box&      valid_state_strip    = temp_state_strip & domain;
        const BoxArray  uncovered_outflow_ba = BoxLib::complementIn(valid_state_strip,Lgrids);

        BL_ASSERT( !(uncovered_outflow_ba.size() &&
                     BoxLib::intersect(Lgrids,valid_state_strip).size()) );

        if ( !(uncovered_outflow_ba.size()) && fine_level[iface] == -1) {
            int ii = icount[lev];
            outFacesAtThisLevel[lev][ii] = outFaces[iface];
            state_strip[lev][ii] = temp_state_strip;
            fine_level[iface] = lev;
            icount[lev]++;
        }
      }
    }

    for (int lev = f_lev; lev >= c_lev; lev--) {
      numOutFlowFaces[lev] = icount[lev];
    }

    NavierStokes* ns0 = dynamic_cast<NavierStokes*>(&LevelData[c_lev]);
    BL_ASSERT(!(ns0 == 0));
   
    int Divu_Type, Divu;
    Real gravity = 0;

    if (which_call == INITIAL_SYNC || which_call == INITIAL_VEL)
    {
      gravity = 0;
      if (!LevelData[c_lev].isStateVariable("divu", Divu_Type, Divu))
        BoxLib::Error("Projection::set_outflow_bcs: No divu.");
    }

    if (which_call == INITIAL_PRESS || which_call == LEVEL_PROJ)
    {
      gravity = ns0->getGravity();
      if (!LevelData[c_lev].isStateVariable("divu", Divu_Type, Divu) &&
          (gravity == 0) )
        BoxLib::Error("Projection::set_outflow_bcs: No divu or gravity.");
    }

    for (int lev = c_lev; lev <= f_lev; lev++) 
    {
      if (numOutFlowFaces[lev] > 0) 
        set_outflow_bcs_at_level (which_call,lev,c_lev,
                                  state_strip[lev],
                                  outFacesAtThisLevel[lev],
                                  numOutFlowFaces[lev],
                                  phi,
                                  Vel_in[lev],
                                  Divu_in[lev],
                                  Sig_in[lev],
                                  have_divu,
                                  gravity);
                                  
    }

}

void
Projection::set_outflow_bcs_at_level (int          which_call,
                                      int          lev,
                                      int          c_lev,
                                      Box*         state_strip,
                                      Orientation* outFacesAtThisLevel,
                                      int          numOutFlowFaces,
                                      MultiFab**   phi, 
                                      MultiFab*    Vel_in,
                                      MultiFab*    Divu_in,
                                      MultiFab*    Sig_in,
                                      int          have_divu,
                                      Real         gravity)
{
    BL_ASSERT(dynamic_cast<NavierStokes*>(&LevelData[lev]) != 0);

    Box domain = parent->Geom(lev).Domain();

    const int ncStripWidth = 1;

    FArrayBox  rho[2*BL_SPACEDIM];
    FArrayBox dsdt[2*BL_SPACEDIM];
    FArrayBox dudt[1][2*BL_SPACEDIM];
    FArrayBox phi_fine_strip[2*BL_SPACEDIM];

    BoxArray grown_grids(Sig_in->boxArray());
    grown_grids.grow(1);
    MultiFab Sig_grown(grown_grids,1,0);
    for (MFIter mfi(Sig_grown); mfi.isValid(); ++mfi)
        Sig_grown[mfi].copy((*Sig_in)[mfi]);

    for (int iface = 0; iface < numOutFlowFaces; iface++)
    {
        dsdt[iface].resize(state_strip[iface],1);
        dudt[0][iface].resize(state_strip[iface],BL_SPACEDIM);

        rho[iface].resize(state_strip[iface],1);

        Sig_grown.copy(rho[iface]);

        Box phi_strip = 
            BoxLib::surroundingNodes(BoxLib::bdryNode(domain,
                                                      outFacesAtThisLevel[iface],
                                                      ncStripWidth));
        phi_fine_strip[iface].resize(phi_strip,1);
        phi_fine_strip[iface].setVal(0.);
    }
    Sig_grown.clear();

    ProjOutFlowBC projBC;
    if (which_call == INITIAL_PRESS) 
    {

        const int*      lo_bc = phys_bc->lo();
        const int*      hi_bc = phys_bc->hi();
        projBC.computeRhoG(rho,phi_fine_strip,
                           parent->Geom(lev),
                           outFacesAtThisLevel,numOutFlowFaces,gravity,
                           lo_bc,hi_bc);
    }
    else
    {
        Vel_in->FillBoundary();
        //
        // Build a new MultiFab for which the cells outside the domain
        // are in the valid region instead of being ghost cells, so that
        // we can copy these values into the dudt array.
        //
        BoxArray grown_vel_ba = Vel_in->boxArray();
        grown_vel_ba.grow(1);
        MultiFab grown_vel(grown_vel_ba,BL_SPACEDIM,0);
        for (MFIter vmfi(*Vel_in); vmfi.isValid(); ++vmfi)
            grown_vel[vmfi].copy((*Vel_in)[vmfi]);

        if (have_divu)
        {
            for (int iface = 0; iface < numOutFlowFaces; iface++) 
                grown_vel.copy(dudt[0][iface]);
            //
            // Reuse grown_vel to fill dsdt.
            //
            for (MFIter vmfi(*Vel_in); vmfi.isValid(); ++vmfi)
                grown_vel[vmfi].copy((*Divu_in)[vmfi],0,0,1);

            for (int iface = 0; iface < numOutFlowFaces; iface++) 
                grown_vel.copy(dsdt[iface],0,0,1);
        }
        else
        {
            for (int iface = 0; iface < numOutFlowFaces; iface++) 
            {
                grown_vel.copy(dudt[0][iface]);
                dsdt[iface].setVal(0);
            }
        }

        const int*      lo_bc = phys_bc->lo();
        const int*      hi_bc = phys_bc->hi();
        projBC.computeBC(dudt, dsdt, rho, phi_fine_strip,
                         parent->Geom(lev),
                         outFacesAtThisLevel,
                         numOutFlowFaces, lo_bc, hi_bc, gravity);
    }

    for (int i = 0; i < 2*BL_SPACEDIM; i++)
    {
        rho[i].clear();
        dsdt[i].clear();
        dudt[0][i].clear();
    }

    for ( int iface = 0; iface < numOutFlowFaces; iface++)
    {
        BoxArray phi_fine_strip_ba(phi_fine_strip[iface].box());
        MultiFab phi_fine_strip_mf(phi_fine_strip_ba,1,0);

        for (MFIter mfi(phi_fine_strip_mf); mfi.isValid(); ++mfi)
            phi_fine_strip_mf[mfi].copy(phi_fine_strip[iface]);

        phi[lev]->copy(phi_fine_strip_mf);
    }

    if (lev > c_lev) 
    {
        putDown(phi, phi_fine_strip, c_lev, lev, outFacesAtThisLevel,
                numOutFlowFaces, ncStripWidth);
    }
}


//
// Given vel, rhs & sig, this solves Div (sig * Grad phi) = Div vel + rhs.
// On return, vel becomes vel  - sig * Grad phi.
//
void Projection::doNodalProjection(int c_lev, int nlevel, 
				   MultiFab* vel[], MultiFab* phi[], MultiFab* sig[],
				   MultiFab* rhs_cc[], const PArray<MultiFab>& rhnd, 
				   Real rel_tol, Real abs_tol,
				   MultiFab* sync_resid_crse,
				   MultiFab* sync_resid_fine)
{
  BL_PROFILE("Projection:::doNodalProjection()");

  int f_lev = c_lev + nlevel - 1;

  BL_ASSERT(vel[c_lev]->nGrow() == 1);
  BL_ASSERT(vel[f_lev]->nGrow() == 1);
  BL_ASSERT(phi[c_lev]->nGrow() == 1);
  BL_ASSERT(phi[f_lev]->nGrow() == 1);
  BL_ASSERT(sig[c_lev]->nGrow() == 1);
  BL_ASSERT(sig[f_lev]->nGrow() == 1);

  BL_ASSERT(sig[c_lev]->nComp() == 1);
  BL_ASSERT(sig[f_lev]->nComp() == 1);

  if (sync_resid_crse != 0) { 
    BL_ASSERT(nlevel == 1);
    BL_ASSERT(c_lev < parent->finestLevel());
  }

  if (sync_resid_fine != 0) { 
    BL_ASSERT((nlevel == 1 || nlevel == 2));
    BL_ASSERT(c_lev > 0);
  }

  if (rhs_cc[c_lev] != 0) {
    if (rhs_cc[c_lev]->box(0).type() == IntVect::TheNodeVector()) {
      BoxLib::Abort("Projection::doNodalProjection: rhs_cc cannot be nodal type");
    }
    BL_ASSERT(rhs_cc[c_lev]->nGrow() == 1);
    BL_ASSERT(rhs_cc[f_lev]->nGrow() == 1);
  }

  MultiFab* vold[maxlev] = {0};
  if (sync_resid_fine !=0 || sync_resid_crse != 0) {
    vold[c_lev] = new MultiFab(parent->boxArray(c_lev), BL_SPACEDIM, 1);
    MultiFab::Copy(*vold[c_lev], *vel[c_lev], 0, 0, BL_SPACEDIM, 1);

    bool inflowCorner = false;
    set_boundary_velocity(c_lev, 1, vold, inflowCorner);
  }

  set_boundary_velocity(c_lev, nlevel, vel, true);

  int lo_inflow[3] = {0};
  int hi_inflow[3] = {0};

  const int* lo_bc = phys_bc->lo();
  const int* hi_bc = phys_bc->hi();
  
  for (int idir=0; idir<BL_SPACEDIM; idir++) {
    if (lo_bc[idir] == Inflow) {
      lo_inflow[idir] = 1;
    }
    if (hi_bc[idir] == Inflow) {
      hi_inflow[idir] = 1;
    }
  }

  std::vector<Geometry> mg_geom(nlevel);
  for (int lev = 0; lev < nlevel; lev++) {
    mg_geom[lev] = parent->Geom(lev+c_lev);
  }  

  int mg_bc[2*BL_SPACEDIM];
  for ( int i = 0; i < BL_SPACEDIM; ++i ) {
    if ( mg_geom[0].isPeriodic(i) ) {
      mg_bc[i*2 + 0] = 0;
      mg_bc[i*2 + 1] = 0;
    }
    else {
      mg_bc[i*2 + 0] = phys_bc->lo(i)==Outflow? MGT_BC_DIR : MGT_BC_NEU;
      mg_bc[i*2 + 1] = phys_bc->hi(i)==Outflow? MGT_BC_DIR : MGT_BC_NEU;
    }
  }

  std::vector<BoxArray> mg_grids(nlevel);
  for (int lev = 0; lev < nlevel; lev++) {
    mg_grids[lev] = parent->boxArray(lev+c_lev);
  }

  std::vector<DistributionMapping> dmap(nlevel);
  for (int lev=0; lev < nlevel; lev++ ) {
    dmap[lev] = LevelData[lev+c_lev].get_new_data(State_Type).DistributionMap();
  }

  bool nodal = true;

  const MultiFab* csig[maxlev];
  for (int lev = 0; lev < nlevel; lev++) {
    csig[lev] = sig[lev+c_lev];
  }

  bool have_rhcc;
  if (rhs_cc[c_lev] == 0) {
    have_rhcc = false;
  }
  else {
    have_rhcc = false;
    for (int lev=c_lev; lev<=f_lev; lev++) {
      if (rhs_cc[lev]->norm0() != 0.0) {
	have_rhcc = true;
	break;
      }
    }
  }

  MGT_Solver mgt_solver(mg_geom, mg_bc, mg_grids, dmap, nodal, hg_stencil, have_rhcc,
                        0, 1, P_code);

  mgt_solver.set_nodal_coefficients(csig);

  mgt_solver.nodal_project(&phi[c_lev], &vel[c_lev], &rhs_cc[c_lev], rhnd, 
			   rel_tol, abs_tol, &lo_inflow[0], &hi_inflow[0]);  

  // Must fill sync_resid_fine before sync_resid_crse because of the side effecs in the calls.

  if (sync_resid_fine != 0) {
    const BoxArray& levelGrids = mg_grids[0];
    const Geometry& levelGeom = mg_geom[0];

    MultiFab msk(levelGrids, 1, 1); 

    mask_grids(msk, levelGrids, levelGeom);

    sync_resid_fine->setVal(0.0, sync_resid_fine->nGrow());

    int isCoarse = 0;
    mgt_solver.fill_sync_resid(sync_resid_fine, msk, *vold[c_lev], isCoarse);
  }

  if (sync_resid_crse != 0) {  // only level solve will come to here
    const BoxArray& fineGrids = parent->boxArray(c_lev+1);
    const BoxArray& levelGrids = mg_grids[0];
    const Geometry& levelGeom = mg_geom[0];
    IntVect ref_ratio = parent->refRatio(c_lev);

    MultiFab msk(levelGrids, 1, 1); 

    mask_grids(msk, levelGrids, levelGeom, fineGrids, ref_ratio);

    sync_resid_crse->setVal(0.0, sync_resid_crse->nGrow());

    int isCoarse = 1;
    mgt_solver.fill_sync_resid(sync_resid_crse, msk, *vold[c_lev], isCoarse);
  }

  delete vold[c_lev];

  if (verbose >= 1)
    MGT_Solver::FlushFortranOutput();
}


void
Projection::mask_grids (MultiFab& msk, const BoxArray& grids, const Geometry& geom,
                        const BoxArray& fineGrids, const IntVect& ref_ratio)
{
  BL_PROFILE("Projection::mask_grids(1)");

  BoxArray localfine = fineGrids;
  localfine.coarsen(ref_ratio);

  msk.setBndry(BogusValue);

  const int* lo_bc = phys_bc->lo();
  const int* hi_bc = phys_bc->hi();
  const Box& domainBox = geom.Domain();

  std::vector< std::pair<int,Box> > isects;

  for (MFIter mfi(msk); mfi.isValid(); ++mfi) {
    int i = mfi.index();

    FArrayBox& msk_fab = msk[mfi];

    const Box& reg  = grids[i]; 
    msk_fab.setVal(1.0, reg, 0); 

    for (int idir=0; idir<BL_SPACEDIM; idir++) {
      if (lo_bc[idir] == Inflow) {
	if (reg.smallEnd(idir) == domainBox.smallEnd(idir)) {
	  Box bx = BoxLib::adjCellLo(reg, idir);
	  msk_fab.setVal(1.0, bx, 0);
	}
      }
      if (hi_bc[idir] == Inflow) {
	if (reg.bigEnd(idir) == domainBox.bigEnd(idir)) {
	  Box bx = BoxLib::adjCellHi(reg, idir);
	  msk_fab.setVal(1.0, bx, 0);
	}
      }
    }

    localfine.intersections(reg,isects);

    for (int ii = 0; ii < isects.size(); ii++) {
      const Box& fbox = isects[ii].second;
      msk_fab.setVal(0.0, fbox, 0);

      for (int idir=0; idir<BL_SPACEDIM; idir++) {
	if (lo_bc[idir] == Inflow) {
	  if (fbox.smallEnd(idir) == domainBox.smallEnd(idir)) {
	    Box bx = BoxLib::adjCellLo(fbox, idir);
	    msk_fab.setVal(0.0, bx, 0);
	  }
	}
	if (hi_bc[idir] == Inflow) {
	  if (fbox.bigEnd(idir) == domainBox.bigEnd(idir)) {
	    Box bx = BoxLib::adjCellHi(fbox, idir);
	    msk_fab.setVal(0.0, bx, 0);
	  }
	}
      }      
    }
  }

  msk.FillBoundary();
  if (geom.isAnyPeriodic()) {
    geom.FillPeriodicBoundary(msk, true); // fill corners too
  }
}

void Projection::mask_grids(MultiFab& msk, const BoxArray& grids, const Geometry& geom)
{
  BL_PROFILE("Projection::mask_grids(2)");

  msk.setBndry(BogusValue);

  const Box& domainBox = geom.Domain();
  IntVect is_periodic(D_DECL(geom.isPeriodic(0),
  			     geom.isPeriodic(1),
  			     geom.isPeriodic(2)));
  Box domainBox_p = BoxLib::grow(domainBox, is_periodic);

  const int* lo_bc = phys_bc->lo();
  const int* hi_bc = phys_bc->hi();

  for (MFIter mfi(msk); mfi.isValid(); ++mfi) {
    int i = mfi.index();
    
    FArrayBox& msk_fab = msk[mfi];
    const Box& fullBox = msk_fab.box(); // including ghost cells

    Box insBox = domainBox_p & fullBox;
    if (! insBox.isEmpty()) {
      msk_fab.setVal(0.0, insBox, 0);
    }

    const Box& regBox  = grids[i]; // interior region    
    msk_fab.setVal(1.0, regBox, 0);    

    for (int idir=0; idir<BL_SPACEDIM; idir++) {
      if (lo_bc[idir] == Inflow) {
	if (regBox.smallEnd(idir) == domainBox.smallEnd(idir)) {
	  Box bx = BoxLib::adjCellLo(regBox, idir);
	  msk_fab.setVal(1.0, bx, 0);
	}
      }
      if (hi_bc[idir] == Inflow) {
	if (regBox.bigEnd(idir) == domainBox.bigEnd(idir)) {
	  Box bx = BoxLib::adjCellHi(regBox, idir);
	  msk_fab.setVal(1.0, bx, 0);
	}
      }
    }
  }

  msk.FillBoundary();
  if (geom.isAnyPeriodic()) {
    geom.FillPeriodicBoundary(msk, true); // fill corners tooo
  }  
}

// set velocity in ghost cells to zero except for inflow
void Projection::set_boundary_velocity(int c_lev, int nlevel, MultiFab* vel[], bool inflowCorner)
{
  const int* lo_bc = phys_bc->lo();
  const int* hi_bc = phys_bc->hi();

  for (int lev=c_lev; lev < c_lev+nlevel; lev++) {
    const BoxArray& grids = parent->boxArray(lev);
    const Box& domainBox = parent->Geom(lev).Domain();

    for (int idir=0; idir<BL_SPACEDIM; idir++) {

      if (lo_bc[idir] != Inflow && hi_bc[idir] != Inflow) {
	vel[lev]->setBndry(0.0, Xvel+idir, 1);
      }
      else {
	for (MFIter mfi(*vel[lev]); mfi.isValid(); ++mfi) {
	  int i = mfi.index();

	  FArrayBox& v_fab = (*vel[lev])[mfi];

	  const Box& reg = grids[i];
	  const Box& bxg1 = BoxLib::grow(reg, 1);

	  BoxList bxlist(reg);

	  if (lo_bc[idir] == Inflow && reg.smallEnd(idir) == domainBox.smallEnd(idir)) {
	    Box bx;
	    if (inflowCorner) {
	      bx = BoxLib::adjCellLo(bxg1, idir);
	      bx.shift(idir, +1);
	    }
	    else {
	      bx = BoxLib::adjCellLo(reg, idir);
	    }
	    bxlist.push_back(bx);
	  }

	  if (hi_bc[idir] == Inflow && reg.bigEnd(idir) == domainBox.bigEnd(idir)) {
	    Box bx;
	    if (inflowCorner) {
	      bx = BoxLib::adjCellHi(bxg1, idir);
	      bx.shift(idir, -1);
	    }
	    else {
	      bx = BoxLib::adjCellHi(reg, idir);
	    }
	    bxlist.push_back(bx);
	  }

	  BoxList bxlist2 = BoxLib::complementIn(bxg1, bxlist);

	  for (BoxList::iterator it=bxlist2.begin(); it != bxlist2.end(); ++it) {
	    v_fab.setVal(0.0, *it, Xvel+idir, 1);
	  }
	}
      }
    }
  }
}
