/*
 * Copyright (c) 2003 Matteo Frigo
 * Copyright (c) 2003 Massachusetts Institute of Technology
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "ct.h"

typedef struct {
     plan_rdft super;
     plan *cld;
     plan *cldw;
     int r;
} P;

static void apply_dit(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     plan_rdft *cld;
     plan_hc2hc *cldw;

     cld = (plan_rdft *) ego->cld;
     cld->apply((plan *) cld, I, O);

     cldw = (plan_hc2hc *) ego->cldw;
     cldw->apply((plan *) cldw, O);
}

static void apply_dif(const plan *ego_, R *I, R *O)
{
     const P *ego = (const P *) ego_;
     plan_rdft *cld;
     plan_hc2hc *cldw;

     cldw = (plan_hc2hc *) ego->cldw;
     cldw->apply((plan *) cldw, I);

     cld = (plan_rdft *) ego->cld;
     cld->apply((plan *) cld, I, O);
}

static void awake(plan *ego_, int flg)
{
     P *ego = (P *) ego_;
     AWAKE(ego->cld, flg);
     AWAKE(ego->cldw, flg);
}

static void destroy(plan *ego_)
{
     P *ego = (P *) ego_;
     X(plan_destroy_internal)(ego->cldw);
     X(plan_destroy_internal)(ego->cld);
}

static void print(const plan *ego_, printer *p)
{
     const P *ego = (const P *) ego_;
     p->print(p, "(rdft-ct-%s/%d%(%p%)%(%p%))",
	      ego->super.apply == apply_dit ? "dit" : "dif",
	      ego->r, ego->cldw, ego->cld);
}

static int applicable0(const ct_solver *ego, const problem *p_, planner *plnr)
{
     if (RDFTP(p_)) {
          const problem_rdft *p = (const problem_rdft *) p_;
	  int r;

          return (1
                  && p->sz->rnk == 1
                  && p->vecsz->rnk <= 1 

		  && (/* either the problem is R2HC, which is solved by DIT */
		       (p->kind[0] == R2HC)
		      ||
		       /* or the problem is HC2R, in which case it is solved
			  by DIF, which destroys the input */
		       (p->kind[0] == HC2R && 
			(p->I == p->O || DESTROY_INPUTP(plnr))))
		  
		  && ((r = X(choose_radix)(ego->r, p->sz->dims[0].n)) > 0)
		  && p->sz->dims[0].n > r);
     }
     return 0;
}


static int applicable(const ct_solver *ego, const problem *p_, planner *plnr)
{
     const problem_rdft *p;

     if (!applicable0(ego, p_, plnr))
          return 0;

     p = (const problem_rdft *) p_;

     /* emulate fftw2 behavior */
     if (NO_VRECURSEP(plnr) && (p->vecsz->rnk > 0))  return 0;

     return 1;
}

static plan *mkplan(const solver *ego_, const problem *p_, planner *plnr)
{
     const ct_solver *ego = (const ct_solver *) ego_;
     const problem_rdft *p;
     P *pln = 0;
     plan *cld = 0, *cldw = 0;
     int n, r, m, vl, ivs, ovs;
     iodim *d;
     tensor *t1, *t2;

     static const plan_adt padt = {
	  X(rdft_solve), awake, print, destroy
     };

     if (!applicable(ego, p_, plnr))
          return (plan *) 0;

     p = (const problem_rdft *) p_;
     d = p->sz->dims;
     n = d[0].n;
     r = X(choose_radix)(ego->r, n);
     m = n / r;

     X(tensor_tornk1)(p->vecsz, &vl, &ivs, &ovs);

     switch (p->kind[0]) {
	 case R2HC:
	      cldw = ego->mkcldw(ego, 
				 R2HC, r, m, d[0].os, vl, ovs, p->O,
				 plnr);
	      if (!cldw) goto nada;

	      t1 = X(mktensor_1d)(r, d[0].is, m * d[0].os);
	      t2 = X(tensor_append)(t1, p->vecsz);
	      X(tensor_destroy)(t1);

	      cld = X(mkplan_d)(plnr, 
				X(mkproblem_rdft_d)(
				     X(mktensor_1d)(m, r * d[0].is, d[0].os),
				     t2, p->I, p->O, p->kind)
		   );
	      if (!cld) goto nada;

	      pln = MKPLAN_RDFT(P, &padt, apply_dit);
	      break;

	 case HC2R:
	      cldw = ego->mkcldw(ego,
				 HC2R, r, m, d[0].is, vl, ivs, p->I,
				 plnr);
	      if (!cldw) goto nada;

	      t1 = X(mktensor_1d)(r, m * d[0].is, d[0].os);
	      t2 = X(tensor_append)(t1, p->vecsz);
	      X(tensor_destroy)(t1);

	      cld = X(mkplan_d)(plnr, 
				X(mkproblem_rdft_d)(
				     X(mktensor_1d)(m, d[0].is, r * d[0].os),
				     t2, p->I, p->O, p->kind)
		   );
	      if (!cld) goto nada;
	      
	      pln = MKPLAN_RDFT(P, &padt, apply_dif);
	      break;

	 default: 
	      A(0);
	      
     }

     pln->cld = cld;
     pln->cldw = cldw;
     pln->r = r;
     X(ops_add)(&cld->ops, &cldw->ops, &pln->super.super.ops);
     return &(pln->super.super);

 nada:
     X(plan_destroy_internal)(cldw);
     X(plan_destroy_internal)(cld);
     return (plan *) 0;
}

ct_solver *X(mksolver_rdft_ct)(size_t size, int r, mkinferior mkcldw)
{
     static const solver_adt sadt = { mkplan };
     ct_solver *slv = (ct_solver *)X(mksolver)(size, &sadt);
     slv->r = r;
     slv->mkcldw = mkcldw;
     return slv;
}

plan *X(mkplan_hc2hc)(size_t size, const plan_adt *adt, hc2hcapply apply)
{
     plan_hc2hc *ego;

     ego = (plan_hc2hc *) X(mkplan)(size, adt);
     ego->apply = apply;

     return &(ego->super);
}

/* generic routine that produces cld0 and cldm, used by inferior
   solvers */
int X(rdft_ct_mkcldrn)(rdft_kind kind, int r, int m, int s, 
		       R *IO, planner *plnr,
		       plan **cld0p, plan **cldmp)
{
     tensor *radix = X(mktensor_1d)(r, m * s, m * s);
     tensor *null = X(mktensor_0d)();
     int imid = s * (m/2);
     plan *cld0 = 0, *cldm = 0;

     A(R2HC_KINDP(kind) || HC2R_KINDP(kind));

     cld0 = X(mkplan_d)(plnr, 
			X(mkproblem_rdft_1)(radix, null, IO, IO, kind));
     if (!cld0) goto nada;

     cldm = X(mkplan_d)(plnr,
			X(mkproblem_rdft_1)(
			     m%2 ? null : radix, null, IO + imid, IO + imid, 
			     R2HC_KINDP(kind) ? R2HCII : HC2RIII));
     if (!cldm) goto nada;

     X(tensor_destroy2)(null, radix);
     *cld0p = cld0;
     *cldmp = cldm;
     return 1;

 nada:
     X(tensor_destroy2)(null, radix);
     X(plan_destroy_internal)(cld0);
     X(plan_destroy_internal)(cldm);
     return 0;
}
