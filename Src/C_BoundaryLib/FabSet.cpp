//BL_COPYRIGHT_NOTICE

//
// $Id: FabSet.cpp,v 1.31 1999-04-08 17:01:45 lijewski Exp $
//

#ifdef BL_USE_NEW_HFILES
#include <list>
using std::list;
#else
#include <list.h>
#endif

#include <FabSet.H>
#include <Looping.H>
#include <RunStats.H>

FabSet::FabSet () {}

FabSet::~FabSet () {}

FabSet::FabSet (const BoxArray& grids, int ncomp)
    :
    MultiFab(grids,ncomp,0,Fab_allocate)
{}

FabSet&
FabSet::copyFrom (const FArrayBox& src)
{
    for (FabSetIterator fsi(*this); fsi.isValid(); ++fsi)
    {
        fsi().copy(src);
    }
    return *this;
}

FabSet&
FabSet::copyFrom (const FArrayBox& src,
                  int              src_comp,
                  int              dest_comp,
                  int              num_comp)
{
    for (FabSetIterator fsi(*this); fsi.isValid(); ++fsi)
    {
        fsi().copy(src,src_comp,dest_comp,num_comp);
    }
    return *this;
}

FabSet&
FabSet::copyFrom (const FArrayBox& src,
                  const Box&       subbox,
                  int              src_comp,
                  int              dest_comp,
                  int              num_comp)
{
    assert(src.box().contains(subbox));

    for (FabSetIterator fsi(*this); fsi.isValid(); ++fsi)
    {
        if (subbox.intersects(fsi().box()))
        {
            Box dbox = fsi().box() & subbox;

            fsi().copy(src,dbox,src_comp,dbox,dest_comp,num_comp);
        }
    }
    return *this;
}

//
// Used in caching CollectData() stuff for copyFrom() and plusFrom().
//

struct FSRec
{
    FSRec ();

    FSRec (const BoxArray& src,
           const BoxArray& dst,
           int             ngrow,
           int             scomp,
           int             dcomp,
           int             ncomp);

    FSRec (const FSRec& rhs);

    ~FSRec ();

    bool operator== (const FSRec& rhs) const;
    bool operator!= (const FSRec& rhs) const { return !operator==(rhs); }

    Array<int>    m_snds;     // Snds cache for CollectData().
    CommDataCache m_commdata; // CommData cache for CollectData().
    BoxArray      m_src;
    BoxArray      m_dst;
    int           m_ngrow;
    int           m_scomp;
    int           m_dcomp;
    int           m_ncomp;
};

FSRec::FSRec ()
    :
    m_ngrow(-1),
    m_scomp(-1),
    m_dcomp(-1),
    m_ncomp(-1)
{}

FSRec::FSRec (const BoxArray& src,
              const BoxArray& dst,
              int             ngrow,
              int             scomp,
              int             dcomp,
              int             ncomp)
    :
    m_src(src),
    m_dst(dst),
    m_ngrow(ngrow),
    m_scomp(scomp),
    m_dcomp(dcomp),
    m_ncomp(ncomp)
{
    assert(ngrow >= 0);
    assert(scomp >= 0);
    assert(dcomp >= 0);
    assert(ncomp >  0);
}

FSRec::FSRec (const FSRec& rhs)
    :
    m_snds(rhs.m_snds),
    m_commdata(rhs.m_commdata),
    m_src(rhs.m_src),
    m_dst(rhs.m_dst),
    m_ngrow(rhs.m_ngrow),
    m_scomp(rhs.m_scomp),
    m_dcomp(rhs.m_dcomp),
    m_ncomp(rhs.m_ncomp)
{}

FSRec::~FSRec () {}

bool
FSRec::operator== (const FSRec& rhs) const
{
    return
        m_ngrow == rhs.m_ngrow &&
        m_scomp == rhs.m_scomp &&
        m_dcomp == rhs.m_dcomp &&
        m_ncomp == rhs.m_ncomp &&
        m_src   == rhs.m_src   &&
        m_dst   == rhs.m_dst;
}

//
// A useful typedef.
//
typedef list<FSRec> FSRecList;

//
// Cache of FSRec info.
//
static FSRecList TheCache;

void
FabSet::FlushCache ()
{
    TheCache.clear();
}

static
FSRec&
TheFSRec (const MultiFab& src,
          const FabSet&   dst,
          int             ngrow,
          int             scomp,
          int             dcomp,
          int             ncomp)
{
    assert(ngrow >= 0);
    assert(scomp >= 0);
    assert(dcomp >= 0);
    assert(ncomp >  0);

    FSRec rec(src.boxArray(),dst.boxArray(),ngrow,scomp,dcomp,ncomp);

    for (FSRecList::iterator it = TheCache.begin(); it != TheCache.end(); ++it)
    {
        if (*it == rec)
        {
            return *it;
        }
    }

    TheCache.push_front(rec);

    return TheCache.front();
}

void
FabSet::DoIt (const MultiFab& src,
              int             ngrow,
              int             scomp,
              int             dcomp,
              int             ncomp,
              How             how)
{
    assert(ngrow <= src.nGrow());
    assert((dcomp+ncomp) <= nComp());
    assert((scomp+ncomp) <= src.nComp());
    assert(how == FabSet::COPYFROM || how == FabSet::PLUSFROM);

    FArrayBox            tmp;
    FabSetCopyDescriptor fscd;
    vector<FillBoxId>    fbids;
    const int            MyProc = ParallelDescriptor::MyProc();
    MultiFabId           mfid   = fscd.RegisterFabArray(const_cast<MultiFab*>(&src));
    FSRec&               rec    = TheFSRec(src,*this,ngrow,scomp,dcomp,ncomp);

    for (FabSetIterator fsi(*this); fsi.isValid(); ++fsi)
    {
       for (int i = 0; i < src.length(); i++)
       {
           Box ovlp = fsi().box() & ::grow(src.boxArray()[i],ngrow);

            if (ovlp.ok())
            {
                fbids.push_back(fscd.AddBox(mfid,
                                            ovlp,
                                            0,
                                            i,
                                            scomp,
                                            how == COPYFROM ? dcomp : 0,
                                            ncomp,
                                            false));

                assert(fbids.back().box() == ovlp);
                //
                // Also save the index of our FAB needing filling.
                //
                fbids.back().FabIndex(fsi.index());
            }
        }
    }

    fscd.CollectData(&rec.m_snds,&rec.m_commdata);

    for (int i = 0; i < fbids.size(); i++)
    {
        assert(DistributionMap()[fbids[i].FabIndex()] == MyProc);

        if (how == COPYFROM)
        {
            fscd.FillFab(mfid, fbids[i], (*this)[fbids[i].FabIndex()]);
        }
        else
        {
            tmp.resize(fbids[i].box(), ncomp);

            fscd.FillFab(mfid, fbids[i], tmp);

            (*this)[fbids[i].FabIndex()].plus(tmp,tmp.box(),0,dcomp,ncomp);
        }
    }
}

FabSet&
FabSet::copyFrom (const MultiFab& src,
                  int             ngrow,
                  int             scomp,
                  int             dcomp,
                  int             ncomp)
{
    static RunStats stats("fabset_copyfrom");

    stats.start();

    DoIt(src,ngrow,scomp,dcomp,ncomp,FabSet::COPYFROM);

    stats.end();

    return *this;
}

FabSet&
FabSet::plusFrom (const MultiFab& src,
                  int             ngrow,
                  int             scomp,
                  int             dcomp,
                  int             ncomp)
{
    static RunStats stats("fabset_plusfrom");

    stats.start();

    DoIt(src,ngrow,scomp,dcomp,ncomp,FabSet::PLUSFROM);

    stats.end();

    return *this;
}

//
// Linear combination this := a*this + b*src
// Note: corresponding fabsets must be commensurate.
//
FabSet&
FabSet::linComb (Real          a,
                 Real          b,
                 const FabSet& src,
                 int           scomp,
                 int           dcomp,
                 int           ncomp)
{
    assert(length() == src.length());

    for (FabSetIterator fsi(*this); fsi.isValid(); ++fsi)
    {
        DependentFabSetIterator dfsi(fsi, src);

        assert(fsi().box() == dfsi().box());
        //
        // WARNING: same fab used as src and dest here.
        //
        fsi().linComb(fsi(),
                      fsi().box(),
                      dcomp,
                      dfsi(),
                      dfsi().box(),
                      scomp,
                      a,
                      b,
                      fsi().box(),
                      dcomp,
                      ncomp);
    }
    return *this;
}

FabSet&
FabSet::linComb (Real            a,
                 const MultiFab& mfa,
                 int             a_comp,
                 Real            b,
                 const MultiFab& mfb,
                 int             b_comp,
                 int             dcomp,
                 int             ncomp,
                 int             ngrow)
{
    assert(ngrow <= mfa.nGrow());
    assert(ngrow <= mfb.nGrow());

    static RunStats stats("fabset_lincomb");

    stats.start();

    const BoxArray& bxa = mfa.boxArray();
    const BoxArray& bxb = mfb.boxArray();

    assert(bxa == bxb);

    MultiFabCopyDescriptor mfcd;

    MultiFabId mfid_mfa = mfcd.RegisterFabArray(const_cast<MultiFab*>(&mfa));
    MultiFabId mfid_mfb = mfcd.RegisterFabArray(const_cast<MultiFab*>(&mfb));

    vector<FillBoxId> fbids_mfa, fbids_mfb;

    for (FabSetIterator fsi(*this); fsi.isValid(); ++fsi)
    {
        for (int grd = 0; grd < bxa.length(); grd++)
        {
            Box ovlp = fsi().box() & ::grow(bxa[grd],ngrow);

            if (ovlp.ok())
            {
                fbids_mfa.push_back(mfcd.AddBox(mfid_mfa,
                                                ovlp,
                                                0,
                                                grd,
                                                a_comp,
                                                0,
                                                ncomp,
                                                false));

                assert(fbids_mfa.back().box() == ovlp);
                //
                // Also save the index of the FAB in the FabSet.
                //
                fbids_mfa.back().FabIndex(fsi.index());

                fbids_mfb.push_back(mfcd.AddBox(mfid_mfb,
                                                ovlp,
                                                0,
                                                grd,
                                                b_comp,
                                                0,
                                                ncomp,
                                                false));

                assert(fbids_mfb.back().box() == ovlp);
            }
        }
    }

    mfcd.CollectData();

    FArrayBox a_fab, b_fab;

    const int MyProc = ParallelDescriptor::MyProc();

    assert(fbids_mfa.size() == fbids_mfb.size());

    for (int i = 0; i < fbids_mfa.size(); i++)
    {
        a_fab.resize(fbids_mfa[i].box(), ncomp);
        b_fab.resize(fbids_mfb[i].box(), ncomp);

        mfcd.FillFab(mfid_mfa, fbids_mfa[i], a_fab);
        mfcd.FillFab(mfid_mfb, fbids_mfb[i], b_fab);

        assert(DistributionMap()[fbids_mfa[i].FabIndex()] == MyProc);

        (*this)[fbids_mfa[i].FabIndex()].linComb(a_fab,
                                                 fbids_mfa[i].box(),
                                                 0,
                                                 b_fab,
                                                 fbids_mfa[i].box(),
                                                 0,
                                                 a,
                                                 b,
                                                 fbids_mfa[i].box(),
                                                 dcomp,
                                                 ncomp);
    }

    stats.end();

    return *this;
}
