
#include <DiffusedIB.H>

#include <AMReX_ParmParse.H>
#include <AMReX_TagBox.H>
#include <AMReX_Utility.H>
#include <AMReX_PhysBCFunct.H>
#include <AMReX_MLNodeLaplacian.H>
#include <AMReX_FillPatchUtil.H>
#include <iamr_constants.H>

using namespace amrex;

void nodal_phi_to_pvf(MultiFab& pvf, const MultiFab& phi_nodal)
{

    Print() << "In the nodal_phi_to_pvf " << std::endl;

#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
    for (MFIter mfi(pvf,TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.tilebox();
        auto const& pvffab   = pvf.array(mfi);
        auto const& pnfab = phi_nodal.array(mfi);
        amrex::ParallelFor(bx, [pvffab, pnfab]
        AMREX_GPU_DEVICE(int i, int j, int k) noexcept
        {
            Real num = 0.0;
            for(int kk=k; kk<=k+1; kk++) {
                for(int jj=j; jj<=j+1; jj++) {
                    for(int ii=i; ii<=i+1; ii++) {
                        num += (-pnfab(ii,jj,kk)) * nodal_phi_to_heavi(-pnfab(ii,jj,kk));
                    }
                }
            }
            Real deo = 0.0;
            for(int kk=k; kk<=k+1; kk++) {
                for(int jj=j; jj<=j+1; jj++) {
                    for(int ii=i; ii<=i+1; ii++) {
                        deo += std::abs(pnfab(ii,jj,kk));
                    }
                }
            }
            pvffab(i,j,k) = num / (deo + 1.e-12);
        });
    }

}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                     other function                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

AMREX_GPU_HOST_DEVICE
[[nodiscard]] AMREX_FORCE_INLINE
Real cal_momentum(Real rho, Real radious)
{
    return 8.0 * Math::pi<Real>() * rho * Math::powi<5>(radious) / 15.0;
}

AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
void amrex::deltaFunction(int xf, Real h, Real& value)
{
    Real rr = amrex::Math::abs(xf);
    if(rr >=0 && rr < 1){
        value = 1.0 / 8.0 * ( 3.0 - 2.0 * rr + std::sqrt( 1.0 + 4 * rr - 4 * Math::powi<2>(rr))) / h;
    }else if (rr >= 1 && rr < 2) {
        value = 1.0 / 8.0 * ( 5.0 - 2.0 * rr + std::sqrt( -7.0 + 12 * rr - 4 * Math::powi<2>(rr))) / h;
    }else {
        value = 0;
    }
}

template <typename P>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
void deposit_cic (P const& p,
                  ParticleReal fxP,
                  ParticleReal fyP,
                  ParticleReal fzP,
                  Array4<Real> const& E,
                  GpuArray<Real,AMREX_SPACEDIM> const& plo,
                  GpuArray<Real,AMREX_SPACEDIM> const& dx,
                  const deltaFuncType& delta)
{
    const Real d = AMREX_D_TERM(dx[0], *dx[1], *dx[2]);

    Real lx = (p.pos(0) - plo[0]) / dx[0];
    Real ly = (p.pos(1) - plo[1]) / dx[1];
    Real lz = (p.pos(2) - plo[2]) / dx[2];

    int i = static_cast<int>(Math::floor(lx));
    int j = static_cast<int>(Math::floor(ly));
    int k = static_cast<int>(Math::floor(lz));
    // calc_delta(i, j, k, dxi, rho);
    for(int ii = -2; ii < 3; ii++){
        for(int jj = -2; jj < 3; jj++){
            for(int kk = -2; kk < 3; kk ++){
                E(i + ii, j + jj, k + kk, 3) = 0;
                E(i + ii, j + jj, k + kk, 4) = 0;
                E(i + ii, j + jj, k + kk, 5) = 0;
                Real tU, tV, tW;
                delta(ii, dx[0], tU);
                delta(jj, dx[1], tV);
                delta(kk, dx[2], tW);
                Real delta_value = tU * tV * tW;
                Gpu::Atomic::AddNoRet(&E(i + ii, j + jj, k + kk, 3), delta_value * fxP * d);
                Gpu::Atomic::AddNoRet(&E(i + ii, j + jj, k + kk, 4), delta_value * fyP * d);
                Gpu::Atomic::AddNoRet(&E(i + ii, j + jj, k + kk, 5), delta_value * fzP * d);
            }
        }
    }
}

template <typename P = Particle<numAttri>>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
void interpolate_cir(P const& p, Real& Up, Real& Vp, Real& Wp,
                     Array4<Real const> const& E,
                     GpuArray<Real, AMREX_SPACEDIM> const& plo,
                     GpuArray<Real, AMREX_SPACEDIM> const& dx,
                     const deltaFuncType& delta)
{
    const Real d = AMREX_D_TERM(dx[0], *dx[1], *dx[2]);

    const Real lx = (p.pos(0) - plo[0]) / dx[0]; // x
    const Real ly = (p.pos(1) - plo[1]) / dx[1]; // y
    const Real lz = (p.pos(2) - plo[2]) / dx[2]; // z

    int i = static_cast<int>(Math::floor(lx)); // i
    int j = static_cast<int>(Math::floor(ly)); // j
    int k = static_cast<int>(Math::floor(lz)); // k
    //Eularian velocity interpolate to Largrangian Marker's velocity
    for(int ii = -2; ii < 3; ii++){
        for(int jj = -2; jj < 3; jj++){
            for(int kk = -2; kk < 3; kk ++){
                Up = 0;
                Vp = 0;
                Wp = 0;
                Real tU, tV, tW;
                delta(ii, dx[0], tU);
                delta(jj, dx[1], tV);
                delta(kk, dx[2], tW);
                Real delta_value = tU * tV * tW;
                Gpu::Atomic::AddNoRet(&Up, delta_value * E(i + ii, j + jj, k + kk, 0) * d);
                Gpu::Atomic::AddNoRet(&Vp, delta_value * E(i + ii, j + jj, k + kk, 1) * d);
                Gpu::Atomic::AddNoRet(&Wp, delta_value * E(i + ii, j + jj, k + kk, 2) * d);
            }
        }
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                    mParticle member function                  */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void mParticle::InitParticlesAndMarkers(const Vector<Real>& x,
                                        const Vector<Real>& y,
                                        const Vector<Real>& z,
                                        int radious){
    //get particle tile
    std::pair<int, int> key{0,0};
    auto& particleTileTmp = GetParticles(0)[key];
    //insert markers
    if ( ParallelDescriptor::MyProc() == ParallelDescriptor::IOProcessorNumber() ) {
        //insert particle's markers
        //just initial particle, not markers
        Real phiK = 0;
        Real h = m_gdb->Geom(level).CellSizeArray()[0];
        int Ml = static_cast<int>(Math::pi<Real>() / 3 * (12 * Math::powi<2>(RADIOUS / h)));
        Real dv = Math::pi<Real>() * h / 3 / Ml * (12 * RADIOUS * RADIOUS + h * h);
        // int Ml = 32;
        Print() << "\n initial the particle, and the particle marker's : " << Ml << ", dv : " << dv << "\n";
        for(int marker_index = 0; marker_index < Ml; marker_index++){
            //insert code
            ParticleType markerP;
            markerP.id() = ParticleType::NextID();
            markerP.cpu() = ParallelDescriptor::MyProc();
            //calculate the position of marker
            Real Hk = -1.0 + 2.0 * (marker_index) / ( Ml - 1.0);
            Real thetaK = std::acos(Hk);
            if(marker_index == 0 || marker_index == ( Ml - 1)){
                phiK = 0;
            }else {
                phiK = std::fmod( phiK + 3.809 / std::sqrt(Ml) / std::sqrt( 1 - Math::powi<2>(Hk)) , 2 * Math::pi<Real>());
            }
            // Print() << "Marker index : " << marker_index << ", Hk : " << Hk << ", thetak : " << thetaK << ", phiK : " << phiK << "\n";
            markerP.pos(0) = 0.5 + RADIOUS * std::sin(thetaK) * std::cos(phiK);
            markerP.pos(1) = 0.5 + RADIOUS * std::sin(thetaK) * std::sin(phiK);
            markerP.pos(2) = 0.5 + RADIOUS * std::cos(thetaK);

            std::array<ParticleReal, numAttri> Marker_attr;
            Marker_attr[U_Marker] = 1.0;
            Marker_attr[V_Marker] = 1.0;
            Marker_attr[W_Marker] = 1.0;
            Marker_attr[Fx_Marker] = 3.0;
            Marker_attr[Fy_Marker] = 1.0;
            Marker_attr[Fz_Marker] = 3.0;
            // attr[V] = 10.0;
            particleTileTmp.push_back(markerP);
            particleTileTmp.push_back_real(Marker_attr);
        }

        kernel mKernel;
        mKernel.location << 0.5, 0.5, 0.5;
        mKernel.velocity << 3.0, 1.0, 3.0;
        mKernel.omega << 0.0, 0.0, 0.0;
        mKernel.varphi << 0.0, 0.0, 0.0;
        mKernel.radious = RADIOUS;
        mKernel.ml = Ml;
        mKernel.dv = dv;
        mKernel.rho = 1.0;
        particle_kernels.push_back(mKernel);
        WriteAsciiFile(amrex::Concatenate("particle", 0));
    }
    Redistribute();
}

void mParticle::VelocityInterpolation(const MultiFab &Eular,
                                      const deltaFuncType& delta)//
{
    const int ng = Eular.nGrow();
    const auto& gm = m_gdb->Geom(level);
    auto plo = gm.ProbLoArray();
    auto dx = gm.CellSizeArray();
    //assert
    AMREX_ASSERT(OnSameGrids(level, *Eular[0]));

    for(mParIter pti(*this, level); pti.isValid(); ++pti){
        auto& particles = pti.GetArrayOfStructs();
        auto *p_ptr = particles.data();
        const Long np = pti.numParticles();

        auto& attri = pti.GetAttribs();
        auto* Up = attri[P_ATTR::U_Marker].data();
        auto* Vp = attri[P_ATTR::V_Marker].data();
        auto* Wp = attri[P_ATTR::W_Marker].data();
        const auto& E = Eular[pti].array();

        amrex::ParallelFor(np, [=] AMREX_GPU_DEVICE (int i) noexcept{
            interpolate_cir(p_ptr[i], Up[i], Vp[i], Wp[i], particle_kernels.at(0).dv, E, plo, dx, delta);
        });
    }
    WriteAsciiFile(amrex::Concatenate("particle", 1));
}

void mParticle::ForceSpreading(MultiFab & Eular, 
                               const deltaFuncType& delta){
    amrex::Print() << "\nfine_level : " << level
                   << ",  tU's size : " << Eular.size()
                   << ",  tU n grow : " << Eular.nGrow() << "\n";
    const int ng = Eular.nGrow();
    int index = 0;
    const auto& gm = m_gdb->Geom(level);
    auto plo = gm.ProbLoArray();
    auto dxi = gm.CellSizeArray();
    for(mParIter pti(*this, level); pti.isValid(); ++pti){
        const Long np = pti.numParticles();
        const auto& fxP = pti.GetStructOfArrays().GetRealData(P_ATTR::U_Marker);//Fx_Marker 
        const auto& fyP = pti.GetStructOfArrays().GetRealData(P_ATTR::V_Marker);//Fy_Marker 
        const auto& fzP = pti.GetStructOfArrays().GetRealData(P_ATTR::W_Marker);//Fz_Marker 
        const auto& particles = pti.GetArrayOfStructs();
        auto Uarray = Eular[pti].array();

        const auto& fxP_ptr = fxP.data();
        const auto& fyP_ptr = fyP.data();
        const auto& fzP_ptr = fzP.data();
        const auto& p_ptr = particles().data();
        amrex::ParallelFor(np, [=] AMREX_GPU_DEVICE (int i) noexcept{
            deposit_cic(p_ptr[i], fxP_ptr[i], fyP_ptr[i], fzP_ptr[i], particle_kernels.at(0).dv, Uarray, plo, dxi, delta);
        });
    }
}

void mParticle::VelocityCorrection(amrex::MultiFab & Eular, Real dt, Real rhoS)
{
    //NS_LS line 29
    //        const Box& bx = mfi.growntilebox(); ghost cell
        const auto& gm = m_gdb->Geom(level);
    auto plo = gm.ProbLoArray();
    auto dxi = gm.InvCellSizeArray();
    //update the kernel's infomation and cal body force
    int cal_index = 0;
    for(auto & kernel : particle_kernels){
        for(mParIter pti(*this, level); pti.isValid(); ++pti){
            auto &particles = pti.GetArrayOfStructs();
            auto *p_ptr = particles.data();
            auto &attri = pti.GetAttribs();
            auto *FxP = attri[P_ATTR::Fx_Marker].data();
            auto *FyP = attri[P_ATTR::Fy_Marker].data();
            auto *FzP = attri[P_ATTR::Fz_Marker].data();
            auto *UP  = attri[P_ATTR::U_Marker].data();
            auto *VP  = attri[P_ATTR::V_Marker].data();
            auto *WP  = attri[P_ATTR::W_Marker].data();
            const int numOfMarker = kernel.ml;
            const int Dv = kernel.dv;
            const Long np = pti.numParticles();

            mVector ForceDv{0.0,0.0,0.0};
            auto *ForceDv_ptr = &ForceDv;
            mVector Moment{0.0,0.0,0.0};
            auto *Moment_ptr = &Moment;
            auto *location_ptr = &kernel.location;
            auto *omega_ptr = &kernel.omega;
            const Real rho_p = kernel.rho;
            //sum
            amrex::ParallelFor(np, [=] AMREX_GPU_DEVICE (int i) noexcept{
                //calculate the force
                //find current particle's lagrangian marker
                if(p_ptr[i].id() < (cal_index + numOfMarker) && p_ptr[i].id() >= cal_index){
                    *ForceDv_ptr += mVector(p_ptr[i].rdata(P_ATTR::Fx_Marker),
                                            p_ptr[i].rdata(P_ATTR::Fy_Marker),
                                            p_ptr[i].rdata(P_ATTR::Fz_Marker)) * Dv;
                    *Moment_ptr +=  (*location_ptr - mVector(p_ptr[i].pos(0),
                                                             p_ptr[i].pos(1),
                                                             p_ptr[i].pos(2))).cross(
                                    mVector(p_ptr[i].rdata(P_ATTR::Fx_Marker),
                                            p_ptr[i].rdata(P_ATTR::Fy_Marker),
                                            p_ptr[i].rdata(P_ATTR::Fz_Marker))) * Dv;
                }
            });
            mVector oldVelocity = kernel.velocity;
            mVector oldOmega = kernel.omega;
            kernel.velocity = kernel.velocity -
                              2 * alpha_k * dt / Dv / (kernel.rho - 1) * (ForceDv + mVector(0.0, -9.8, 0.0));
            kernel.omega = kernel.omega -
                           2 * alpha_k * dt * kernel.rho / cal_momentum(kernel.rho, kernel.radious) / (kernel.rho - 1) * Moment;
            kernel.location = kernel.location + alpha_k * dt * (kernel.velocity + oldVelocity);
            kernel.varphi = kernel.varphi + alpha_k * dt * (kernel.omega + oldOmega);
            //sum
            auto Uarray = Eular[pti].array();
            amrex::ParallelFor(np, [=] AMREX_GPU_DEVICE (int i) noexcept{
                //calculate the force
                //find current particle's lagrangian marker
                if(p_ptr[i].id() < (cal_index + numOfMarker) && p_ptr[i].id() >= cal_index){
                    mVector tmp = (*omega_ptr).cross(mVector(*location_ptr - mVector(p_ptr[i].pos(0),
                                                                                              p_ptr[i].pos(1),
                                                                                              p_ptr[i].pos(2))));
                    p_ptr[i].rdata(P_ATTR::Fx_Marker) = rho_p / dt *(p_ptr[i].rdata(P_ATTR::U_Marker) + tmp(0));
                    p_ptr[i].rdata(P_ATTR::Fy_Marker) = rho_p / dt *(p_ptr[i].rdata(P_ATTR::V_Marker) + tmp(1));
                    p_ptr[i].rdata(P_ATTR::Fz_Marker) = rho_p / dt *(p_ptr[i].rdata(P_ATTR::W_Marker) + tmp(2));
                }
            });
        }
        cal_index++;
    }
    //Lagrangian to Eular
    ForceSpreading(Eular);
}