/*++
Copyright (c) 2014 Microsoft Corporation

Module Name:

    fpa_solver.h

Abstract:

    Floating-Point Theory Plugin

Author:

    Christoph (cwinter) 2014-04-23

Revision History:

--*/
#pragma once

#include "sat/smt/euf_solver.h"
#include "ast/fpa/fpa2bv_converter.h"
#include "ast/fpa/fpa2bv_rewriter.h"

namespace fpa {

    typedef euf::enode enode;
    typedef euf::theory_var theory_var;

    class solver : public euf::th_euf_solver {
    protected:
        th_rewriter               m_th_rw;
        fpa2bv_converter_wrapped  m_converter;
        fpa2bv_rewriter           m_rw;
        fpa_util                & m_fpa_util;
        bv_util                 & m_bv_util;
        arith_util              & m_arith_util;
        obj_map<expr, expr*>      m_conversions;
        obj_hashtable<func_decl>  m_is_added_to_model;

        bool visit(expr* e) override;
        bool visited(expr* e) override;
        bool post_visit(expr* e, bool sign, bool root) override;

        expr_ref convert(expr* e);
        sat::literal_vector mk_side_conditions();
        void attach_new_th_var(enode* n);
        void activate(expr* e);
        void ensure_equality_relation(theory_var x, theory_var y);
        expr* bv2rm_value(expr* b);
        expr* bvs2fpa_value(sort* s, expr* a, expr* b, expr* c);


    public:
        solver(euf::solver& ctx);
        ~solver() override;

        void asserted(sat::literal l) override;
        void new_eq_eh(euf::th_eq const& eq) override;
        bool use_diseqs() const override { return true; }
        void new_diseq_eh(euf::th_eq const& eq) override;

        sat::literal internalize(expr* e, bool sign, bool root, bool learned) override;
        void internalize(expr* e, bool redundant) override;
        void apply_sort_cnstr(euf::enode* n, sort* s) override;

        std::ostream& display(std::ostream& out) const override;

        void add_value(euf::enode* n, model& mdl, expr_ref_vector& values) override;
        void add_dep(euf::enode* n, top_sort<euf::enode>& dep) override;
    };

}

