/**
 * @file Quadrature.inl
 * @author Jordan Carlson (jwgcarlson@gmail.com)
 * @brief Templated routines for numerical quadrature. */

#include <cassert>
#include <cfloat>
#include <cstdlib>
#include <map>

#include "Common.h"

const int GK_LIMIT = 8192;

const int GM_MINPTS = 1000;
const int GM_MAXPTS = 10000000;


/* GKWorkspace: Workspace for adaptive Gauss-Kronrod algorithm */
struct GKWorkspace {
    const int limit;
    int size;
    double* alist;
    double* blist;
    double* rlist;
    double* elist;
    int* order;

    /* Initialize workspace.  limit is the maximum number of subintervals allowed. */
    GKWorkspace(int limit = GK_LIMIT);
    ~GKWorkspace();

    void GetMaxInterval(double& a, double& b, double& r, double& e);
    void Sort();
    void Update(double a1, double b1, double area1, double error1,
                double a2, double b2, double area2, double error2);
    void SumResults(double& result, double& abserr);

    static void ReportError(int error_code);

    /* General Gauss-Kronrod rule */
    static double GaussKronrod(const int n, const double wg[], const double wgk[],
                               double fc, double fv1[], double fv2[], double half,
                               double* abserr, double* resabs, double* resasc);

    /* 15-point Gauss-Kronrod rule with error estimate */
    template<typename Function>
    static double GaussKronrod15(Function f, double a, double b, double* abserr, double* resabs, double* resasc);

    /* Integrate f(x) from a to b */
    template<typename Function>
    double Integrate(Function f, double a, double b, double epsrel, double epsabs, double* abserr, int* neval);

    static const double wg15[4];
    static const double wgk15[8];
    static const double xgk15[8];
};


/***** GKWorkspace *****/

template<typename Function>
double GKWorkspace::GaussKronrod15(Function f, double a, double b,
                                    double* abserr, double* resabs, double* resasc)
{
    const int n = 8;
    double fv1[n], fv2[n];
    const double c = 0.5*(a + b);         // center of the interval
    const double fc = f(c);               // f evaluated at c
    const double half = 0.5*(b - a);      // half the interval
    double abscissa;
    for(int j = 0; j < (n-1)/2; j++) {
        abscissa = half*xgk15[2*j+1];
        fv1[2*j+1] = f(c - abscissa);
        fv2[2*j+1] = f(c + abscissa);
    }
    for(int j = 0; j < n/2; j++) {
        abscissa = half*xgk15[2*j];
        fv1[2*j] = f(c - abscissa);
        fv2[2*j] = f(c + abscissa);
    }
    return GaussKronrod(n, wg15, wgk15, fc, fv1, fv2, half, abserr, resabs, resasc);
}

/* Adaptive integration using GaussKronrod15 for fundamental intervals */
template<typename Function>
double GKWorkspace::Integrate(Function f, double a, double b, double epsrel, double epsabs, double* pabserr, int* pneval) {
    int error_code = 0;
    int neval = 0;
    double result = 0, abserr = 0, resabs = 0, resasc = 0;

    /* Make sure precision request is reasonable */
    if(epsabs <= 0 && (epsrel < 50*DBL_EPSILON || epsrel < 0.5e-28)) {
        ReportError(1);
        return 0;
    }

    /* Integrate from left to right, fixing the sign at the end */
    double sign = +1;
    if(a > b) {
        double tmp = a;
        a = b;
        b = tmp;
        sign = -1;
    }

    /* Perform first integration */
    result = GaussKronrod15(f, a, b, &abserr, &resabs, &resasc);
    neval += 15;

    /* Initialize workspace */
    size = 1;
    alist[0] = a;
    blist[0] = b;
    rlist[0] = result;
    elist[0] = abserr;
    order[0] = 0;

    double area = result;
    double errsum = abserr;
    int iteration = 1;

    /* Adaptively subdivide interval until convergence is achieved */
    double a1, b1, a2, b2;
    double a_i, b_i, r_i, e_i;
    double area1, area2, area12;
    double error1, error2, error12;
    double resasc1, resasc2, resabs1, resabs2;
    double tolerance;
    do {
        area1 = area2 = area12 = error1 = error2 = error12 = 0; 

        /* Bisect the interval with the largest error estimate */
        GetMaxInterval(a_i, b_i, r_i, e_i);
        a1 = a_i;
        b1 = 0.5*(a_i + b_i);
        a2 = b1;
        b2 = b_i;

        /* Integrate over the two subintervals */
        area1 = GaussKronrod15(f, a1, b1, &error1, &resabs1, &resasc1);
        area2 = GaussKronrod15(f, a2, b2, &error2, &resabs2, &resasc2);
        neval += 30;
        area12 = area1 + area2;
        error12 = error1 + error2;
        errsum += error12 - e_i;
        area += area12 - r_i;

        tolerance = fmax(epsabs, epsrel*fabs(area));

        if(errsum > tolerance) {
            /* Check for bad behavior at a point of the integration range */
            double tmp = (1 + 100*DBL_EPSILON) * (fabs(a2) + 1000*DBL_MIN);
            if(fabs(a1) <= tmp && fabs(b2) <= tmp)
                error_code = 3;

            /* Check if we've reached the maximum number of subintervals */
            if(iteration == limit)
                error_code = 4;
        }

        Update(a1, b1, area1, error1, a2, b2, area2, error2);
        iteration++;
    }
    while(iteration < limit && error_code == 0 && errsum > tolerance);

    /* Re-sum results to minimize round-off error */
    SumResults(result, abserr);

    if(abserr > tolerance)
        ReportError(error_code);

    if(pabserr)
        *pabserr = abserr;
    if(pneval)
        *pneval = neval;
    return sign*result;
}


/* GMWorkspace: workspace for n-dimensional Genz-Malik algorithm */
struct GMWorkspace {
    int n;              // dimension of integral
    void* wrkstr;       // storage space for region objects

    GMWorkspace(int n);
    ~GMWorkspace();
};


/* Singleton class for managing workspaces.  The rationale here is that we
 * might need to call Integrate() many many times, and we don't want to have to
 * allocate and free workspace memory each time.  So instead we keep a list
 * of already allocated workspaces, and a flag to indicate whether or not it's
 * currently in use.  When a new workspace is requested, we first check for
 * an existing unused workspace.  Only if no workspaces are available do we
 * allocate memory for a new one.  This class handles all of this logic. */
struct WorkspaceManager {
    typedef std::map<GKWorkspace*, int> GKWorkspaceList;
    typedef std::map<GMWorkspace*, int> GMWorkspaceList;

    /* List of instantiated workspaces.  The key is a pointer to a workspace
     * object; the value is either 1 if the workspace is currently in use, or
     * 0 otherwise. */
    GKWorkspaceList gk_workspaces;
    GMWorkspaceList gm_workspaces;

    /* Get or release a GKWorkspace for 1-dimensional integration. */
    GKWorkspace* get_gk_workspace();
    void release_gk_workspace(GKWorkspace* workspace);

    /* Get or release a GMWorkspace for n-dimensional integration. */
    GMWorkspace* get_gm_workspace(int n);
    void release_gm_workspace(GMWorkspace* workspace);

    WorkspaceManager();
    ~WorkspaceManager();
};

/* Global WorkspaceManager object, constructed at startup */
extern WorkspaceManager workspace_manager;


/***** Integrate *****/

template<typename Function>
double Integrate(Function f, double a, double b, double epsrel, double epsabs, double* abserr, int* neval) {
    GKWorkspace* workspace = workspace_manager.get_gk_workspace();
    double result = workspace->Integrate(f, a, b, epsrel, epsabs, abserr, neval);
    workspace_manager.release_gk_workspace(workspace);
    return result;
}


/***** Integrate<Sub> *****/

template<typename Function, typename Sub>
struct SubFunc {
    Function f;
    Sub sub;
    SubFunc(Function f_, Sub sub_ = Sub()) : f(f_), sub(sub_) {}
    double operator()(double u) { return f(sub.x(u)) * sub.dxdu(u); }
};

template<typename Sub, typename Function>
double Integrate(Function f, double a, double b, double epsrel, double epsabs, double* abserr, int* neval, Sub sub) {
    GKWorkspace* workspace = workspace_manager.get_gk_workspace();
    SubFunc<Function, Sub> F(f, sub);
    double result = workspace->Integrate(F, sub.u(a), sub.u(b), epsrel, epsabs, abserr, neval);
    workspace_manager.release_gk_workspace(workspace);
    return result;
}