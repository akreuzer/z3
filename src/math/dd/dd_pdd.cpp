/*++
Copyright (c) 2019 Microsoft Corporation

Module Name:

    dd_pdd.cpp

Abstract:

    Poly DD package 

Author:

    Nikolaj Bjorner (nbjorner) 2019-12-17

Revision History:

--*/

#include "util/trace.h"
#include "util/stopwatch.h"
#include "math/dd/dd_pdd.h"

namespace dd {

    pdd_manager::pdd_manager(unsigned num_vars) {
        m_spare_entry = nullptr;
        m_max_num_pdd_nodes = 1 << 24; // up to 16M nodes
        m_mark_level = 0;
        alloc_free_nodes(1024 + num_vars);
        m_disable_gc = false;
        m_is_new_node = false;
        m_mod2_semantics = false;

        // add dummy nodes for operations, and 0, 1 pdds.
        const_info info;
        init_value(info, rational::zero()); // becomes pdd_zero
        init_value(info, rational::one());  // becomes pdd_one
        m_nodes[0].m_refcount = max_rc;
        m_nodes[1].m_refcount = max_rc;
        for (unsigned i = 2; i <= pdd_no_op + 2; ++i) {
            m_nodes.push_back(pdd_node(0,0,0));
            m_nodes.back().m_refcount = max_rc;
            m_nodes.back().m_index = m_nodes.size()-1;
        }
        
        // add variables
        for (unsigned i = 0; i < num_vars; ++i) {
            reserve_var(i);
        }            
    }

    pdd_manager::~pdd_manager() {
        if (m_spare_entry) {
            m_alloc.deallocate(sizeof(*m_spare_entry), m_spare_entry);
        }
        for (auto* e : m_op_cache) {
            SASSERT(e != m_spare_entry);
            m_alloc.deallocate(sizeof(*e), e);
        }
    }

    pdd pdd_manager::add(pdd const& a, pdd const& b) { return pdd(apply(a.root, b.root, pdd_add_op), this); }
    pdd pdd_manager::sub(pdd const& a, pdd const& b) { pdd m(minus(b)); return pdd(apply(a.root, m.root, pdd_add_op), this); }
    pdd pdd_manager::mul(pdd const& a, pdd const& b) { return pdd(apply(a.root, b.root, pdd_mul_op), this); }
    pdd pdd_manager::reduce(pdd const& a, pdd const& b) { return pdd(apply(a.root, b.root, pdd_reduce_op), this); }
    pdd pdd_manager::mk_val(rational const& r) { return pdd(imk_val(r), this); }
    pdd pdd_manager::mul(rational const& r, pdd const& b) { pdd c(mk_val(r)); return pdd(apply(c.root, b.root, pdd_mul_op), this); }
    pdd pdd_manager::add(rational const& r, pdd const& b) { pdd c(mk_val(r)); return pdd(apply(c.root, b.root, pdd_add_op), this); }
    pdd pdd_manager::zero() { return pdd(zero_pdd, this); }
    pdd pdd_manager::one() { return pdd(one_pdd, this); }
    
    pdd_manager::PDD pdd_manager::apply(PDD arg1, PDD arg2, pdd_op op) {
        bool first = true;
        SASSERT(well_formed());
        scoped_push _sp(*this);
        while (true) {
            try {
                return apply_rec(arg1, arg2, op);
            }
            catch (const mem_out &) {
                try_gc();
                if (!first) throw;
                first = false;
            }
        }
        SASSERT(well_formed());
        return 0;
    }

    bool pdd_manager::check_result(op_entry*& e1, op_entry const* e2, PDD a, PDD b, PDD c) {
        if (e1 != e2) {
            SASSERT(e2->m_result != null_pdd);
            push_entry(e1);
            e1 = nullptr;
            return true;            
        }
        else {
            e1->m_pdd1 = a;
            e1->m_pdd2 = b;
            e1->m_op = c;
            SASSERT(e1->m_result == null_pdd);
            return false;        
        }
    }

    pdd_manager::PDD pdd_manager::apply_rec(PDD p, PDD q, pdd_op op) {
        switch (op) {
        case pdd_add_op:
            if (is_zero(p)) return q;
            if (is_zero(q)) return p;
            if (is_val(p) && is_val(q)) return imk_val(val(p) + val(q));
            if (!is_val(p) && level(p) < level(q)) std::swap(p, q);
            if (is_val(p)) std::swap(p, q);
            break;
        case pdd_mul_op:
            if (is_zero(p) || is_zero(q)) return zero_pdd;
            if (is_one(p)) return q;
            if (is_one(q)) return p;
            if (is_val(p) && is_val(q)) return imk_val(val(p) * val(q));
            if (!is_val(p) && level(p) < level(q)) std::swap(p, q);
            if (is_val(p)) std::swap(p, q);
            break;
        case pdd_reduce_op:
            if (is_zero(q)) return p;
            if (is_val(p)) return p;
            if (level(p) < level(q)) return p;
            break;
        default:
            UNREACHABLE();
            break;
        }

        op_entry * e1 = pop_entry(p, q, op);
        op_entry const* e2 = m_op_cache.insert_if_not_there(e1);
        if (check_result(e1, e2, p, q, op)) {
            SASSERT(!m_free_nodes.contains(e2->m_result));
            return e2->m_result;
        }
        PDD r;
        unsigned level_p = level(p), level_q = level(q);
        unsigned npop = 2;
                
        switch (op) {
        case pdd_add_op:
            SASSERT(!is_val(p));
            if (is_val(q)) {
                push(apply_rec(lo(p), q, op));
                r = make_node(level_p, read(1), hi(p));
                npop = 1;
            }
            else if (level_p == level_q) {
                push(apply_rec(lo(p), lo(q), op));
                push(apply_rec(hi(p), hi(q), op));
                r = make_node(level_p, read(2), read(1));                           
            }
            else if (level_p > level_q) {
                push(apply_rec(lo(p), q, op));
                r = make_node(level_p, read(1), hi(p));
                npop = 1;
            }
            else {
                UNREACHABLE();
            }
            break;
        case pdd_mul_op:
            SASSERT(!is_val(p));
            if (is_val(q)) {
                push(apply_rec(lo(p), q, op));
                push(apply_rec(hi(p), q, op));
                r = make_node(level_p, read(2), read(1));
            }
            else if (level_p == level_q) {
                if (m_mod2_semantics) {
                    //
                    // (xa+b)*(xc+d) mod2 == x(ac+bc+ad) + bd
                    //                    == x((a+b)(c+d)+bd) + bd
                    //
                    push(apply_rec(lo(p), lo(q), pdd_mul_op));
                    unsigned bd = read(3);
                    push(apply_rec(hi(p), lo(p), pdd_add_op));
                    push(apply_rec(hi(q), lo(q), pdd_add_op));
                    push(apply_rec(read(1), read(2), pdd_mul_op));
                    push(apply_rec(read(1), bd, pdd_add_op));
                    r = make_node(level_p, bd, read(1));
                    npop = 5;
                }
                else {
                    /*
                      In this case the code should have checked if level(read(1)) == level_a,
                      Then it should have converted read(1) into e := hi(read(1)), f := lo(read(1)),                    
                      Such that read(1) stands for x*e+f.                      
                      The task is then to create the term:                      
                      x*(x*ac + x*e + f) + bd, which is the same as: x*(x*(ac + e) + f) + bd
                    */

                    push(apply_rec(hi(p), hi(q), op));
                    push(apply_rec(hi(p), lo(q), op));
                    push(apply_rec(lo(p), hi(q), op));
                    push(apply_rec(lo(p), lo(q), op));
                    unsigned ac = read(4), ad = read(3), bc = read(2), bd = read(1);
                    push(apply_rec(ad, bc, pdd_add_op));
                    unsigned n = read(1); // n = ad + bc
                    if (!is_val(n) && level(n) == level_p) {
                        push(apply_rec(ac, hi(n), pdd_add_op));
                        push(make_node(level_p, lo(n), read(1)));
                        r = make_node(level_p, bd, read(1));
                        npop = 7;
                    } else {
                        push(make_node(level_p, n, ac));
                        r = make_node(level_p, bd, read(1));
                        npop = 6;
                    }                         
                }
            }
            else {
                // (x*hi(p)+lo(p))*b = x*hi(p)*b + lo(p)*b
                SASSERT(level_p > level_q);
                push(apply_rec(lo(p), q, op));
                push(apply_rec(hi(p), q, op));
                r = make_node(level_p, read(2), read(1));
            }
            break;
        case pdd_reduce_op:
            if (level_p > level_q) {
                push(apply_rec(lo(p), q, op));
                push(apply_rec(hi(p), q, op));
                r = make_node(level_p, read(2), read(1));
            }
            else {
                SASSERT(level_p == level_q);
                r = reduce_on_match(p, q);
                npop = 0;
            }
            break;
        default:
            UNREACHABLE();
        }
        pop(npop);
        e1->m_result = r;

        // SASSERT(well_formed());
        SASSERT(!m_free_nodes.contains(r));
        return r;
    }

    pdd pdd_manager::minus(pdd const& a) {
        if (m_mod2_semantics) {
            return a;
        }
        bool first = true;
        SASSERT(well_formed());
        scoped_push _sp(*this);
        while (true) {
            try {
                return pdd(minus_rec(a.root), this);
            }
            catch (const mem_out &) {
                try_gc();
                if (!first) throw;
                first = false;
            }
        }
        SASSERT(well_formed());        
        return pdd(zero_pdd, this);
    }

    pdd_manager::PDD pdd_manager::minus_rec(PDD a) {
        SASSERT(!m_mod2_semantics);
        if (is_zero(a)) return zero_pdd;
        if (is_val(a)) return imk_val(-val(a));
        op_entry* e1 = pop_entry(a, a, pdd_minus_op);
        op_entry const* e2 = m_op_cache.insert_if_not_there(e1);
        if (check_result(e1, e2, a, a, pdd_minus_op)) 
            return e2->m_result;
        push(minus_rec(lo(a)));
        push(minus_rec(hi(a)));
        PDD r = make_node(level(a), read(2), read(1));
        pop(2);
        e1->m_result = r;
        return r;
    }

    // q = lt(a)/lt(b), return a - b*q
    pdd_manager::PDD pdd_manager::reduce_on_match(PDD a, PDD b) {
        SASSERT(level(a) == level(b) && !is_val(a) && !is_val(b));
        while (lm_divides(b, a)) {
            PDD q = lt_quotient(b, a);
            PDD r = apply(q, b, pdd_mul_op);
            a = apply(a, r, pdd_add_op);
        }
        return a;
    }

    // true if leading monomial of p divides leading monomial of q
    bool pdd_manager::lm_divides(PDD p, PDD q) const {
        while (true) {
            if (is_val(p)) return true;
            if (is_val(q)) return false;
            if (level(p) > level(q)) return false;
            if (level(p) == level(q)) {
                p = hi(p); q = hi(q);
            }
            else {
                q = hi(q);
            }
        }
    }

    // return minus quotient -r, such that lt(q) = lt(p)*r
    // assume lm_divides(p, q)
    pdd_manager::PDD pdd_manager::lt_quotient(PDD p, PDD q) {
        SASSERT(lm_divides(p, q));
        SASSERT(is_val(p) || !is_val(q));
        if (is_val(p)) {
            if (is_val(q)) {
                SASSERT(!val(p).is_zero());
                return imk_val(-val(q)/val(p));
            }     
        }   
        else if (level(p) == level(q)) {
            return lt_quotient(hi(p), hi(q));
        }
        return apply(m_var2pdd[var(q)], lt_quotient(p, hi(q)), pdd_mul_op);
    }

    //
    // p = lcm(lm(a),lm(b))/lm(a), q = lcm(lm(a),lm(b))/lm(b)
    // pc = coeff(lt(a)) qc = coeff(lt(b))
    // compute a*q*qc - b*p*pc
    //
    bool pdd_manager::try_spoly(pdd const& a, pdd const& b, pdd& r) {
        return common_factors(a, b, m_p, m_q, m_pc, m_qc) && (r = spoly(a, b, m_p, m_q, m_pc, m_qc), true);
    }

    pdd pdd_manager::spoly(pdd const& a, pdd const& b, unsigned_vector const& p, unsigned_vector const& q, rational const& pc, rational const& qc) { 
        pdd r1 = mk_val(qc);  
        for (unsigned i = q.size(); i-- > 0; ) r1 = mul(mk_var(q[i]), r1);
        r1 = mul(a, r1);
        pdd r2 = mk_val(-pc);
        for (unsigned i = p.size(); i-- > 0; ) r2 = mul(mk_var(p[i]), r2);
        r2 = mul(b, r2);
        return add(r1, r2);
    }

    bool pdd_manager::common_factors(pdd const& a, pdd const& b, unsigned_vector& p, unsigned_vector& q, rational& pc, rational& qc) { 
        p.reset(); q.reset(); 
        PDD x = a.root, y = b.root;
        bool has_common = false;
        while (true) {
            if (is_val(x) || is_val(y)) {
                if (!has_common) return false;
                while (!is_val(y)) q.push_back(var(y)), y = hi(y);
                while (!is_val(x)) p.push_back(var(x)), x = hi(x);
                pc = val(x);
                qc = val(y);
                if (!m_mod2_semantics && pc.is_int() && qc.is_int()) {
                    rational g = gcd(pc, qc);
                    pc /= g;
                    qc /= g;
                }
                return true;
            }
            if (level(x) == level(y)) {
                has_common = true;
                x = hi(x);
                y = hi(y);
            }
            else if (level(x) > level(y)) {
                p.push_back(var(x));
                x = hi(x);
            }
            else {
                q.push_back(var(y));
                y = hi(y);
            }
        }
    }

    /*
     * Compare leading monomials.
     * The pdd format makes lexicographic comparison easy: compare based on
     * the top variable and descend depending on whether hi(x) == hi(y)
     */
    bool pdd_manager::lt(pdd const& a, pdd const& b) {
        PDD x = a.root;
        PDD y = b.root;
        if (x == y) return false;
        while (true) {
            SASSERT(x != y);
            if (is_val(x)) 
                return !is_val(y) || val(x) < val(y);
            if (is_val(y)) 
                return false;
            if (level(x) == level(y)) {
                if (hi(x) == hi(y)) {
                    x = lo(x);
                    y = lo(y);
                }
                else {
                    x = hi(x);
                    y = hi(y);
                }
            }
            else {
                return level(x) > level(y);
            }
        }
    }

    /**
       Compare leading terms of pdds
     */
    bool pdd_manager::different_leading_term(pdd const& a, pdd const& b) {
        PDD x = a.root;
        PDD y = b.root;
        while (true) {
            if (x == y) return false;
            if (is_val(x) || is_val(y)) return true;
            if (level(x) == level(y)) {
                x = hi(x);
                y = hi(y);
            }
            else {
                return true;
            }
        }
    }

    /*
      Determine whether p is a linear polynomials.
      A linear polynomial is of the form x*v1 + y*v2 + .. + vn,
      where v1, v2, .., vn are values.      
     */
    bool pdd_manager::is_linear(PDD p) {
        while (true) {
            if (is_val(p)) return true;
            if (!is_val(hi(p))) return false;
            p = lo(p);
        }
    }

    bool pdd_manager::is_linear(pdd const& p) { 
        return is_linear(p.root); 
    }

    void pdd_manager::push(PDD b) {
        m_pdd_stack.push_back(b);
    }

    void pdd_manager::pop(unsigned num_scopes) {
        m_pdd_stack.shrink(m_pdd_stack.size() - num_scopes);
    }

    pdd_manager::PDD pdd_manager::read(unsigned index) {
        return m_pdd_stack[m_pdd_stack.size() - index];
    }

    pdd_manager::op_entry* pdd_manager::pop_entry(PDD l, PDD r, PDD op) {
        op_entry* result = nullptr;
        if (m_spare_entry) {
            result = m_spare_entry;
            m_spare_entry = nullptr;
            result->m_pdd1 = l;
            result->m_pdd2 = r;
            result->m_op = op;
        }
        else {
            void * mem = m_alloc.allocate(sizeof(op_entry));
            result = new (mem) op_entry(l, r, op);
        }
        result->m_result = null_pdd;
        return result;
    }

    void pdd_manager::push_entry(op_entry* e) {
        SASSERT(!m_spare_entry);
        m_spare_entry = e;
    }

    pdd_manager::PDD pdd_manager::imk_val(rational const& r) {
        if (r.is_zero()) return zero_pdd;
        if (r.is_one()) return one_pdd;
        if (m_mod2_semantics) return imk_val(mod(r, rational(2)));
        const_info info;
        if (!m_mpq_table.find(r, info)) {
            init_value(info, r);
        }      
        return info.m_node_index;
    }

    void pdd_manager::init_value(const_info& info, rational const& r) {
        unsigned vi = 0;
        if (m_free_values.empty()) {
            vi = m_values.size();
            m_values.push_back(r);
        }
        else {
            vi = m_free_values.back();
            m_free_values.pop_back();
            m_values[vi] = r;
        }

        m_freeze_value = r;
        pdd_node n(vi);
        info.m_value_index = vi;        
        info.m_node_index = insert_node(n);
        m_mpq_table.insert(r, info);
    }

    pdd_manager::PDD pdd_manager::make_node(unsigned lvl, PDD l, PDD h) {
        m_is_new_node = false;
        if (is_zero(h)) return l;
        SASSERT(is_val(l) || level(l) < lvl);
        SASSERT(is_val(h) || level(h) <= lvl);
        pdd_node n(lvl, l, h);
        return insert_node(n);
    }

    pdd_manager::PDD pdd_manager::insert_node(pdd_node const& n) {
        node_table::entry* e = m_node_table.insert_if_not_there2(n);
        if (e->get_data().m_index != 0) {
            unsigned result = e->get_data().m_index;
            SASSERT(well_formed(e->get_data()));
            return result;
        }
        e->get_data().m_refcount = 0;
        bool do_gc = m_free_nodes.empty();
        if (do_gc && !m_disable_gc) {
            gc();
            SASSERT(n.m_hi == 0 || (!m_free_nodes.contains(n.m_hi) && !m_free_nodes.contains(n.m_lo)));            
            e = m_node_table.insert_if_not_there2(n);
            e->get_data().m_refcount = 0;      
        }
        if (do_gc) {
            if (m_nodes.size() > m_max_num_pdd_nodes) {
                throw mem_out();
            }
            alloc_free_nodes(m_nodes.size()/2);
        }
        SASSERT(e->get_data().m_lo == n.m_lo);
        SASSERT(e->get_data().m_hi == n.m_hi);
        SASSERT(e->get_data().m_level == n.m_level);
        SASSERT(!m_free_nodes.empty());
        unsigned result = m_free_nodes.back();
        m_free_nodes.pop_back();
        e->get_data().m_index = result;
        m_nodes[result] = e->get_data();
        SASSERT(well_formed(m_nodes[result]));
        m_is_new_node = true;        
        SASSERT(!m_free_nodes.contains(result));
        SASSERT(m_nodes[result].m_index == result); 
        return result;
    }

    void pdd_manager::try_gc() {
        gc();        
        for (auto* e : m_op_cache) {
            m_alloc.deallocate(sizeof(*e), e);
        }
        m_op_cache.reset();
        SASSERT(m_op_cache.empty());
        SASSERT(well_formed());
    }

    void pdd_manager::reserve_var(unsigned i) {
        while (m_var2level.size() <= i) {
            unsigned v = m_var2level.size();
            m_var2pdd.push_back(make_node(v, zero_pdd, one_pdd));
            m_nodes[m_var2pdd[v]].m_refcount = max_rc;
            m_var2level.push_back(v);
            m_level2var.push_back(v);
        }
    }

    pdd pdd_manager::mk_var(unsigned i) {
        reserve_var(i);
        return pdd(m_var2pdd[i], this);        
    }

    void pdd_manager::set_level2var(unsigned_vector const& level2var) {
        SASSERT(level2var.size() == m_level2var.size());
        for (unsigned i = 0; i < level2var.size(); ++i) {
            m_var2level[level2var[i]] = i;
            m_level2var[i] = level2var[i];
        }
    }
    
    unsigned pdd_manager::dag_size(pdd const& b) {
        init_mark();
        set_mark(0);
        set_mark(1);
        unsigned sz = 0;
        m_todo.push_back(b.root);
        while (!m_todo.empty()) {
            PDD r = m_todo.back();
            m_todo.pop_back();
            if (is_marked(r)) {
                continue;
            }
            ++sz;
            set_mark(r);
            if (is_val(r)) {
                continue;
            }
            if (!is_marked(lo(r))) {
                m_todo.push_back(lo(r));
            }
            if (!is_marked(hi(r))) {
                m_todo.push_back(hi(r));
            }
        }
        return sz;
    }

    unsigned pdd_manager::degree(pdd const& b) {
        init_mark();
        m_degree.reserve(m_nodes.size());
        m_todo.push_back(b.root);
        while (!m_todo.empty()) {
            PDD r = m_todo.back();
            if (is_marked(r)) {
                m_todo.pop_back();
            }
            else if (is_val(r)) {
                m_degree[r] = 0;
                set_mark(r);
            }
            else if (!is_marked(lo(r)) || !is_marked(hi(r))) {
                m_todo.push_back(lo(r));
                m_todo.push_back(hi(r));
            }
            else {
                m_degree[r] = std::max(m_degree[lo(r)], m_degree[hi(r)]+1); 
                set_mark(r);
            }
        }
        return m_degree[b.root];
    }


    double pdd_manager::tree_size(pdd const& p) {
        init_mark();
        m_tree_size.reserve(m_nodes.size());
        m_todo.push_back(p.root);
        while (!m_todo.empty()) {
            PDD r = m_todo.back();
            if (is_marked(r)) {
                m_todo.pop_back();
            }
            else if (is_val(r)) {
                m_tree_size[r] = 1;
                set_mark(r);
            }
            else if (!is_marked(lo(r)) || !is_marked(hi(r))) {
                m_todo.push_back(lo(r));
                m_todo.push_back(hi(r));
            }
            else {
                m_tree_size[r] = 1 + m_tree_size[lo(r)] + m_tree_size[hi(r)]; 
                set_mark(r);
            }
        }
        return m_tree_size[p.root];
    }

    unsigned_vector const& pdd_manager::free_vars(pdd const& p) { 
        init_mark();
        m_free_vars.reset();
        m_todo.push_back(p.root);
        while (!m_todo.empty()) {
            PDD r = m_todo.back();
            m_todo.pop_back();
            if (is_val(r) || is_marked(r)) continue;
            PDD v = m_var2pdd[var(r)];
            if (!is_marked(v)) m_free_vars.push_back(var(r));
            set_mark(r);
            set_mark(v);
            if (!is_marked(lo(r))) m_todo.push_back(lo(r));
            if (!is_marked(hi(r))) m_todo.push_back(hi(r));
        }
        return m_free_vars;
    }


    void pdd_manager::alloc_free_nodes(unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            m_free_nodes.push_back(m_nodes.size());
            m_nodes.push_back(pdd_node());
            m_nodes.back().m_index = m_nodes.size() - 1;
        }        
        m_free_nodes.reverse();
    }

    void pdd_manager::gc() {
        m_free_nodes.reset();
        SASSERT(well_formed());
        IF_VERBOSE(13, verbose_stream() << "(pdd :gc " << m_nodes.size() << ")\n";);
        svector<bool> reachable(m_nodes.size(), false);
        for (unsigned i = m_pdd_stack.size(); i-- > 0; ) {
            reachable[m_pdd_stack[i]] = true;
            m_todo.push_back(m_pdd_stack[i]);
        }
        for (unsigned i = m_nodes.size(); i-- > 2; ) {
            if (m_nodes[i].m_refcount > 0) {
                reachable[i] = true;
                m_todo.push_back(i);
            }
        }
        while (!m_todo.empty()) {
            PDD p = m_todo.back();
            m_todo.pop_back();
            SASSERT(reachable[p]);
            if (is_val(p)) continue;
            if (!reachable[lo(p)]) {
                reachable[lo(p)] = true;
                m_todo.push_back(lo(p));
            }
            if (!reachable[hi(p)]) {
                reachable[hi(p)] = true;
                m_todo.push_back(hi(p));
            }
        }
        for (unsigned i = m_nodes.size(); i-- > 2; ) {
            if (!reachable[i]) {
                if (is_val(i)) {
                    if (m_freeze_value == val(i)) continue;
                    m_free_values.push_back(m_mpq_table.find(val(i)).m_value_index);
                    m_mpq_table.remove(val(i));  
                }
                m_nodes[i].set_internal();
                SASSERT(m_nodes[i].m_refcount == 0);
                m_free_nodes.push_back(i);       
            }
        }
        // sort free nodes so that adjacent nodes are picked in order of use
        std::sort(m_free_nodes.begin(), m_free_nodes.end());
        m_free_nodes.reverse();

        ptr_vector<op_entry> to_delete, to_keep;
        for (auto* e : m_op_cache) {            
            if (e->m_result != null_pdd) {
                to_delete.push_back(e);
            }
            else {
                to_keep.push_back(e);
            }
        }
        m_op_cache.reset();
        for (op_entry* e : to_delete) {
            m_alloc.deallocate(sizeof(*e), e);
        }
        for (op_entry* e : to_keep) {
            m_op_cache.insert(e);
        }

        m_node_table.reset();
        // re-populate node cache
        for (unsigned i = m_nodes.size(); i-- > 2; ) {
            if (reachable[i]) {
                SASSERT(m_nodes[i].m_index == i);
                m_node_table.insert(m_nodes[i]);
            }
        }
        SASSERT(well_formed());
    }

    void pdd_manager::init_mark() {
        m_mark.resize(m_nodes.size());
        ++m_mark_level;
        if (m_mark_level == 0) {
            m_mark.fill(0);
            ++m_mark_level;
        }
    }

    pdd_manager::monomials_t pdd_manager::to_monomials(pdd const& p) {
        if (p.is_val()) {
            std::pair<rational, unsigned_vector> m;
            m.first = p.val();
            monomials_t mons;
            if (!m.first.is_zero()) {
                mons.push_back(m);
            }
            return mons;
        }
        else {
            monomials_t mons = to_monomials(p.hi());
            for (auto & m : mons) {
                m.second.push_back(p.var());
            }
            mons.append(to_monomials(p.lo()));
            return mons;
        }
    }

    std::ostream& pdd_manager::display(std::ostream& out, pdd const& b) {
        auto mons = to_monomials(b);
        bool first = true;
        for (auto& m : mons) {
            if (!first) {
                if (m.first.is_neg()) out << " - ";
                else out << " + ";
            }
            else {
                if (m.first.is_neg()) out << "- ";
            }
            first = false;
            rational c = abs(m.first);
            m.second.reverse();
            if (!c.is_one() || m.second.empty()) {
                out << c;
                if (!m.second.empty()) out << "*";
            }
            bool f = true;
            for (unsigned v : m.second) {
                if (!f) out << "*";
                f = false;
                out << "v" << v;
            }
        }
        return out;
    }

    bool pdd_manager::well_formed() {
        bool ok = true;
        for (unsigned n : m_free_nodes) {
            ok &= (lo(n) == 0 && hi(n) == 0 && m_nodes[n].m_refcount == 0);
            if (!ok) {
                IF_VERBOSE(0, 
                           verbose_stream() << "free node is not internal " << n << " " 
                           << lo(n) << " " << hi(n) << " " << m_nodes[n].m_refcount << "\n";
                           display(verbose_stream()););
                UNREACHABLE();
                return false;
            }
        }
        for (pdd_node const& n : m_nodes) {
            if (!well_formed(n)) {
                IF_VERBOSE(0, display(verbose_stream() << n.m_index << " lo " << n.m_lo << " hi " << n.m_hi << "\n"););
                UNREACHABLE();
                return false;
            }
        }
        return ok;
    }

    bool pdd_manager::well_formed(pdd_node const& n) {
        PDD lo = n.m_lo;
        PDD hi = n.m_hi;        
        if (n.is_internal() || hi == 0) return true;
        bool oklo = is_val(lo) || (level(lo) < n.m_level  && !m_nodes[lo].is_internal());
        bool okhi = is_val(hi) || (level(hi) <= n.m_level && !m_nodes[hi].is_internal());
        return oklo && okhi;
    }

    std::ostream& pdd_manager::display(std::ostream& out) {
        for (unsigned i = 0; i < m_nodes.size(); ++i) {
            pdd_node const& n = m_nodes[i];
            if (i != 0 && n.is_internal()) {
                continue;
            }
            else if (is_val(i)) {
                out << i << " : " << val(i) << "\n";
            }
            else {
                out << i << " : v" << m_level2var[n.m_level] << " " << n.m_lo << " " << n.m_hi << "\n";
            }
        }
        return out;
    }

    pdd& pdd::operator=(pdd const& other) { unsigned r1 = root; root = other.root; m->inc_ref(root); m->dec_ref(r1); return *this; }
    std::ostream& operator<<(std::ostream& out, pdd const& b) { return b.display(out); }

}