// PhoneMirror: BLAS-free build of the Bergamot engine (USE_ONNX_SGEMM).
// Marian links faiss (LSH output layer / random rotations), whose Fortran
// BLAS/LAPACK entry points are never reached when translating with a lexical
// shortlist (the Firefox Translations models).  They abort if ever called.
#include <cstdio>
#include <cstdlib>

#define PM_STUB(name)                                                                  \
    extern "C" int name() {                                                            \
        std::fputs("bergamot: " #name " (BLAS/LAPACK) is not available in this build\n", stderr); \
        std::abort();                                                                  \
    }
PM_STUB(sgemm_)
PM_STUB(dgemm_)
PM_STUB(ssyrk_)
PM_STUB(ssyev_)
PM_STUB(dsyev_)
PM_STUB(sgesvd_)
PM_STUB(dgesvd_)
PM_STUB(sgeqrf_)
PM_STUB(sorgqr_)
