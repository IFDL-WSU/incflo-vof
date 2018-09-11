#include <AMReX_ParmParse.H>

#include <AMReX_BC_TYPES.H>
#include <AMReX_Box.H>
#include <AMReX_EBMultiFabUtil.H>
#include <incflo_F.H>
#include <incflo_eb_F.H>
#include <incflo_icbc_F.H>
#include <incflo_level.H>

std::string incflo_level::load_balance_type = "FixedSize";
std::string incflo_level::knapsack_weight_type = "RunTimeCosts";

// Define unit vectors for easily convert indeces
amrex::IntVect incflo_level::e_x(1, 0, 0);
amrex::IntVect incflo_level::e_y(0, 1, 0);
amrex::IntVect incflo_level::e_z(0, 0, 1);

int incflo_level::m_eb_basic_grow_cells = 2;
int incflo_level::m_eb_volume_grow_cells = 2;
int incflo_level::m_eb_full_grow_cells = 2;
EBSupport incflo_level::m_eb_support_level = EBSupport::full;

incflo_level::~incflo_level(){};

incflo_level::incflo_level()
{
// Geometry on all levels has just been defined in the AmrCore constructor

// No valid BoxArray and DistributionMapping have been defined.
// But the arrays for them have been resized.

#if 0
    int nlevs_max = maxLevel() + 1;
    istep.resize(nlevs_max, 0);
    nsubsteps.resize(nlevs_max, 1);
    for (int lev = 1; lev <= maxLevel(); ++lev) 
        nsubsteps[lev] = MaxRefRatio(lev-1);
#endif
}

void incflo_level::ResizeArrays()
{
	int nlevs_max = maxLevel() + 1;

	p_g.resize(nlevs_max);
	p_go.resize(nlevs_max);

	p0_g.resize(nlevs_max);
	pp_g.resize(nlevs_max);

	ro_g.resize(nlevs_max);
	ro_go.resize(nlevs_max);

	phi.resize(nlevs_max);
	diveu.resize(nlevs_max);

	// RHS and solution arrays for diffusive solve
	rhs_diff.resize(nlevs_max);
	phi_diff.resize(nlevs_max);

	// Current (vel_g) and old (vel_go) velocities
	vel_g.resize(nlevs_max);
	vel_go.resize(nlevs_max);

	// Pressure gradients
	gp.resize(nlevs_max);
	gp0.resize(nlevs_max);

	mu_g.resize(nlevs_max);
	lambda_g.resize(nlevs_max);
	trD_g.resize(nlevs_max);

	// Vorticity
	vort.resize(nlevs_max);

	// MAC velocities used for defining convective term
	m_u_mac.resize(nlevs_max);
	m_v_mac.resize(nlevs_max);
	m_w_mac.resize(nlevs_max);

	xslopes.resize(nlevs_max);
	yslopes.resize(nlevs_max);
	zslopes.resize(nlevs_max);

	bcoeff.resize(nlevs_max);
	for(int i = 0; i < nlevs_max; ++i)
	{
		bcoeff[i].resize(3);
	}

	bcoeff_diff.resize(nlevs_max);
	for(int i = 0; i < nlevs_max; ++i)
	{
		bcoeff_diff[i].resize(3);
	}

	fluid_cost.resize(nlevs_max);

	// EB factory
	ebfactory.resize(nlevs_max);
}

void incflo_level::incflo_set_bc_type(int lev)
{
	Real dx = geom[lev].CellSize(0);
	Real dy = geom[lev].CellSize(1);
	Real dz = geom[lev].CellSize(2);
	Real xlen = geom[lev].ProbHi(0) - geom[lev].ProbLo(0);
	Real ylen = geom[lev].ProbHi(1) - geom[lev].ProbLo(1);
	Real zlen = geom[lev].ProbHi(2) - geom[lev].ProbLo(2);
	Box domain(geom[lev].Domain());

	set_bc_type(bc_ilo.dataPtr(),
				bc_ihi.dataPtr(),
				bc_jlo.dataPtr(),
				bc_jhi.dataPtr(),
				bc_klo.dataPtr(),
				bc_khi.dataPtr(),
				domain.loVect(),
				domain.hiVect(),
				&dx,
				&dy,
				&dz,
				&xlen,
				&ylen,
				&zlen,
				&nghost);
}

void incflo_level::fill_mf_bc(int lev, MultiFab& mf)
{
	Box domain(geom[lev].Domain());

	if(!mf.boxArray().ixType().cellCentered())
		amrex::Error("fill_mf_bc only used for cell-centered arrays!");

	// Impose periodic bc's at domain boundaries and fine-fine copies in the interior
	mf.FillBoundary(geom[lev].periodicity());

// Fill all cell-centered arrays with first-order extrapolation at domain boundaries
#ifdef _OPENMP
#pragma omp parallel
#endif
	for(MFIter mfi(mf, true); mfi.isValid(); ++mfi)
	{
		const Box& sbx = mf[mfi].box();
		fill_bc0(mf[mfi].dataPtr(),
				 sbx.loVect(),
				 sbx.hiVect(),
				 bc_ilo.dataPtr(),
				 bc_ihi.dataPtr(),
				 bc_jlo.dataPtr(),
				 bc_jhi.dataPtr(),
				 bc_klo.dataPtr(),
				 bc_khi.dataPtr(),
				 domain.loVect(),
				 domain.hiVect(),
				 &nghost);
	}
}

//
// Subroutine to compute norm0 of EB multifab
//
Real incflo_level::incflo_norm0(const Vector<std::unique_ptr<MultiFab>>& mf, int lev, int comp)
{
	MultiFab mf_tmp(mf[lev]->boxArray(),
					mf[lev]->DistributionMap(),
					mf[lev]->nComp(),
					0,
					MFInfo(),
					*ebfactory[lev]);

	MultiFab::Copy(mf_tmp, *mf[lev], comp, comp, 1, 0);
	EB_set_covered(mf_tmp, 0.0);

	return mf_tmp.norm0(comp);
}

Real incflo_level::incflo_norm0(MultiFab& mf, int lev, int comp)
{
	MultiFab mf_tmp(mf.boxArray(), mf.DistributionMap(), mf.nComp(), 0, MFInfo(), *ebfactory[lev]);

	MultiFab::Copy(mf_tmp, mf, comp, comp, 1, 0);
	EB_set_covered(mf_tmp, 0.0);

	return mf_tmp.norm0(comp);
}

//
// Subroutine to compute norm1 of EB multifab
//
Real incflo_level::incflo_norm1(const Vector<std::unique_ptr<MultiFab>>& mf, int lev, int comp)
{
	MultiFab mf_tmp(mf[lev]->boxArray(),
					mf[lev]->DistributionMap(),
					mf[lev]->nComp(),
					0,
					MFInfo(),
					*ebfactory[lev]);

	MultiFab::Copy(mf_tmp, *mf[lev], comp, comp, 1, 0);
	EB_set_covered(mf_tmp, 0.0);

	return mf_tmp.norm1(comp, geom[lev].periodicity());
}

Real incflo_level::incflo_norm1(MultiFab& mf, int lev, int comp)
{
	MultiFab mf_tmp(mf.boxArray(), mf.DistributionMap(), mf.nComp(), 0, MFInfo(), *ebfactory[lev]);

	MultiFab::Copy(mf_tmp, mf, comp, comp, 1, 0);
	EB_set_covered(mf_tmp, 0.0);

	return mf_tmp.norm1(comp, geom[lev].periodicity());
}

void incflo_level::incflo_compute_vort(int lev)
{
	BL_PROFILE("incflo_level::incflo_compute_vort");
	Box domain(geom[lev].Domain());

#ifdef _OPENMP
#pragma omp parallel
#endif
	for(MFIter mfi(*vel_g[lev], true); mfi.isValid(); ++mfi)
	{
		// Tilebox
		Box bx = mfi.tilebox();

		// This is to check efficiently if this tile contains any eb stuff
		const EBFArrayBox& vel_fab = dynamic_cast<EBFArrayBox const&>((*vel_g[lev])[mfi]);
		const EBCellFlagFab& flags = vel_fab.getEBCellFlagFab();

		if(flags.getType(amrex::grow(bx, 0)) == FabType::regular)
		{
			compute_vort(BL_TO_FORTRAN_BOX(bx),
						 BL_TO_FORTRAN_ANYD((*vort[lev])[mfi]),
						 BL_TO_FORTRAN_ANYD((*vel_g[lev])[mfi]),
						 geom[lev].CellSize());
		}
		else
		{
			vort[lev]->setVal(0.0, bx, 0, 1);
		}
	}
}
