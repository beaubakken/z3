/*++
Copyright (c) 2020 Microsoft Corporation

Module Name:

    euf_egraph.cpp

Abstract:

    E-graph layer

Author:

    Nikolaj Bjorner (nbjorner) 2020-08-23

--*/

#include "ast/euf/euf_egraph.h"
#include "ast/ast_pp.h"
#include "ast/ast_ll_pp.h"
#include "ast/ast_translation.h"

namespace euf {

    void egraph::undo_eq(enode* r1, enode* n1, unsigned r2_num_parents) {
        enode* r2 = r1->get_root();
        r2->dec_class_size(r1->class_size());
        std::swap(r1->m_next, r2->m_next);
        auto begin = r2->begin_parents() + r2_num_parents, end = r2->end_parents();
        for (auto it = begin; it != end; ++it) 
            m_table.erase(*it);
        for (enode* c : enode_class(r1)) 
            c->m_root = r1;
        for (auto it = begin; it != end; ++it) 
            m_table.insert(*it);
        r2->m_parents.shrink(r2_num_parents);
        unmerge_justification(n1);
    }

    enode* egraph::mk_enode(expr* f, unsigned num_args, enode * const* args) {
        enode* n = enode::mk(m_region, f, num_args, args);
        m_nodes.push_back(n);
        m_exprs.push_back(f);
        m_expr2enode.setx(f->get_id(), n, nullptr);
        push_node(n);
        for (unsigned i = 0; i < num_args; ++i)
            set_merge_enabled(args[i], true);
        return n;
    }

    void egraph::reinsert(enode* n) {
        unsigned num_parents = n->m_parents.size();
        for (unsigned i = 0; i < num_parents; ++i) {
            enode* p = n->m_parents[i];
            if (is_equality(p)) {
                reinsert_equality(p);
            }
            else {
                auto rc = m_table.insert(p);
                merge(rc.first, p, justification::congruence(rc.second));
                if (inconsistent())
                    break;
            }
        }
    }

    void egraph::reinsert_equality(enode* p) {
        SASSERT(is_equality(p));
        if (p->get_arg(0)->get_root() == p->get_arg(1)->get_root() && m_value(p) != l_true) {
            add_literal(p, true);
        }
    }

    bool egraph::is_equality(enode* p) const {
        return m.is_eq(p->get_expr());
    }

    void egraph::force_push() {
        if (m_num_scopes == 0)
            return;
        for (; m_num_scopes > 0; --m_num_scopes) {
            m_scopes.push_back(m_updates.size());
            m_region.push_scope();
        }
        m_updates.push_back(update_record(m_new_th_eqs_qhead, update_record::new_th_eq_qhead()));
        m_updates.push_back(update_record(m_new_lits_qhead, update_record::new_lits_qhead()));
        SASSERT(m_new_lits_qhead <= m_new_lits.size());
        SASSERT(m_new_th_eqs_qhead <= m_new_th_eqs.size());
    }

    void egraph::update_children(enode* n) {
        for (enode* child : enode_args(n)) 
            child->get_root()->add_parent(n);
        n->set_update_children();            
    }

    enode* egraph::mk(expr* f, unsigned num_args, enode *const* args) {
        SASSERT(!find(f));
        force_push();
        enode *n = mk_enode(f, num_args, args);
        SASSERT(n->class_size() == 1);        
        if (num_args == 0 && m.is_unique_value(f))
            n->mark_interpreted();
        if (num_args == 0) 
            return n;
        if (is_equality(n)) {
            update_children(n);
            reinsert_equality(n);
            return n;
        }
        enode_bool_pair p = m_table.insert(n);
        enode* n2 = p.first;
        if (n2 == n) 
            update_children(n);        
        else 
            merge(n, n2, justification::congruence(p.second));        
        return n;
    }

    egraph::egraph(ast_manager& m) : m(m), m_table(m), m_exprs(m) {
        m_tmp_eq = enode::mk_tmp(m_region, 2);
    }

    egraph::~egraph() {
        for (enode* n : m_nodes) 
            n->m_parents.finalize();
    }

    void egraph::add_th_eq(theory_id id, theory_var v1, theory_var v2, enode* c, enode* r) {
        TRACE("euf_verbose", tout << "eq: " << v1 << " == " << v2 << "\n";);
        m_new_th_eqs.push_back(th_eq(id, v1, v2, c, r));
        m_updates.push_back(update_record(update_record::new_th_eq()));
        ++m_stats.m_num_th_eqs;
    }

    void egraph::add_th_diseq(theory_id id, theory_var v1, theory_var v2, expr* eq) {
        if (!th_propagates_diseqs(id))
            return;
        TRACE("euf_verbose", tout << "eq: " << v1 << " != " << v2 << "\n";);
        m_new_th_eqs.push_back(th_eq(id, v1, v2, eq));
        m_updates.push_back(update_record(update_record::new_th_eq()));
        ++m_stats.m_num_th_diseqs;
    }

    void egraph::add_literal(enode* n, bool is_eq) {
        TRACE("euf_verbose", tout << "lit: " << n->get_expr_id() << "\n";);
        m_new_lits.push_back(enode_bool_pair(n, is_eq));
        m_updates.push_back(update_record(update_record::new_lit()));
        if (is_eq) ++m_stats.m_num_eqs; else ++m_stats.m_num_lits;
    }

    void egraph::new_diseq(enode* n1) {
        SASSERT(m.is_eq(n1->get_expr()));
        enode* arg1 = n1->get_arg(0), * arg2 = n1->get_arg(1);
        enode* r1 = arg1->get_root();
        enode* r2 = arg2->get_root();
        TRACE("euf", tout << "new-diseq:  " << mk_pp(r1->get_expr(), m) << " " << mk_pp(r2->get_expr(), m) << ": " << r1->has_th_vars() << " " << r2->has_th_vars() << "\n";);
        if (r1 == r2)
            return;
        if (!r1->has_th_vars())
            return;
        if (!r2->has_th_vars())
            return;
        if (r1->has_one_th_var() && r2->has_one_th_var() && r1->get_first_th_id() == r2->get_first_th_id()) {
            theory_id id = r1->get_first_th_id();
            if (!th_propagates_diseqs(id))
                return;
            theory_var v1 = arg1->get_closest_th_var(id);
            theory_var v2 = arg2->get_closest_th_var(id);
            add_th_diseq(id, v1, v2, n1->get_expr());
            return;
        }
        for (auto p : euf::enode_th_vars(r1)) {
            if (!th_propagates_diseqs(p.get_id()))
                continue;
            for (auto q : euf::enode_th_vars(r2))
                if (p.get_id() == q.get_id()) 
                    add_th_diseq(p.get_id(), p.get_var(), q.get_var(), n1->get_expr());
        }
    }


    /*
    * Propagate disequalities over equality atoms that are assigned to false.
    */

    void egraph::add_th_diseqs(theory_id id, theory_var v1, enode* r) {
        SASSERT(r->is_root());
        if (!th_propagates_diseqs(id))
            return;
        for (enode* p : enode_parents(r)) {
            if (m.is_eq(p->get_expr()) && m.is_false(p->get_root()->get_expr())) {
                enode* n = nullptr;
                n = (r == p->get_arg(0)->get_root()) ? p->get_arg(1) : p->get_arg(0);
                n = n->get_root();
                theory_var v2 = n->get_closest_th_var(id);
                if (v2 != null_theory_var)
                    add_th_diseq(id, v1, v2, p->get_expr());                        
            }
        }
    }

    void egraph::set_th_propagates_diseqs(theory_id id) {
        m_th_propagates_diseqs.reserve(id + 1, false);
        m_th_propagates_diseqs[id] = true;
    }

    bool egraph::th_propagates_diseqs(theory_id id) const {
        return m_th_propagates_diseqs.get(id, false);
    }



    void egraph::add_th_var(enode* n, theory_var v, theory_id id) {
        force_push();
        theory_var w = n->get_th_var(id);
        enode* r = n->get_root();

        if (w == null_theory_var) {
            n->add_th_var(v, id, m_region);
            m_updates.push_back(update_record(n, id, update_record::add_th_var()));
            if (r != n) {
                theory_var u = r->get_th_var(id);
                if (u == null_theory_var) {
                    r->add_th_var(v, id, m_region);
                    add_th_diseqs(id, v, r);
                }
                else 
                    add_th_eq(id, v, u, n, r);                
            }
        }
        else {
            theory_var u = r->get_th_var(id);
            SASSERT(u != v && u != null_theory_var);
            n->replace_th_var(v, id);
            m_updates.push_back(update_record(n, id, u, update_record::replace_th_var()));
            add_th_eq(id, v, u, n, r);
        }
    }

    void egraph::undo_add_th_var(enode* n, theory_id tid) {
        theory_var v = n->get_th_var(tid);
        SASSERT(v != null_theory_var);
        n->del_th_var(tid);
        enode* root = n->get_root();
        if (root != n && root->get_th_var(tid) == v)
            root->del_th_var(tid);
    }

    void egraph::set_merge_enabled(enode* n, bool enable_merge) {
        if (enable_merge != n->merge_enabled()) {
            m_updates.push_back(update_record(n, update_record::toggle_merge()));
            n->set_merge_enabled(enable_merge);
        }
    }

    void egraph::pop(unsigned num_scopes) {
        if (num_scopes <= m_num_scopes) {
            m_num_scopes -= num_scopes;
            return;
        }
        num_scopes -= m_num_scopes;
        m_num_scopes = 0;
        
        SASSERT(m_new_lits_qhead <= m_new_lits.size());
        unsigned old_lim = m_scopes.size() - num_scopes;
        unsigned num_updates = m_scopes[old_lim];
        auto undo_node = [&]() {
            enode* n = m_nodes.back();
            expr* e = m_exprs.back();
            if (n->num_args() > 0)
                m_table.erase(n);
            m_expr2enode[e->get_id()] = nullptr;
            n->~enode();
            m_nodes.pop_back();
            m_exprs.pop_back();
        };
        for (unsigned i = m_updates.size(); i-- > num_updates; ) {
            auto const& p = m_updates[i];
            switch (p.tag) {
            case update_record::tag_t::is_add_node:
                undo_node();
                break;
            case update_record::tag_t::is_toggle_merge:
                p.r1->set_merge_enabled(!p.r1->merge_enabled());
                break;
            case update_record::tag_t::is_set_parent:
                undo_eq(p.r1, p.n1, p.r2_num_parents);
                break;
            case update_record::tag_t::is_add_th_var:
                undo_add_th_var(p.r1, p.r2_num_parents);
                break;
            case update_record::tag_t::is_replace_th_var:
                SASSERT(p.r1->get_th_var(p.m_th_id) != null_theory_var);
                p.r1->replace_th_var(p.m_old_th_var, p.m_th_id);
                break;
            case update_record::tag_t::is_new_lit:
                m_new_lits.pop_back();
                break;
            case update_record::tag_t::is_new_th_eq:
                m_new_th_eqs.pop_back();
                break;
            case update_record::tag_t::is_new_th_eq_qhead:
                m_new_th_eqs_qhead = p.qhead;
                break;
            case update_record::tag_t::is_new_lits_qhead:
                m_new_lits_qhead = p.qhead;
                break;
            case update_record::tag_t::is_inconsistent:
                m_inconsistent = p.m_inconsistent;
                break;
            default:
                UNREACHABLE();
                break;
            }                
        }        
       
        m_updates.shrink(num_updates);
        m_scopes.shrink(old_lim);        
        m_region.pop_scope(num_scopes);  
        m_worklist.reset();
        SASSERT(m_new_lits_qhead <= m_new_lits.size());
        SASSERT(m_new_th_eqs_qhead <= m_new_th_eqs.size());
    }

    void egraph::merge(enode* n1, enode* n2, justification j) {
        SASSERT(m.get_sort(n1->get_expr()) == m.get_sort(n2->get_expr()));
        enode* r1 = n1->get_root();
        enode* r2 = n2->get_root();
        if (r1 == r2)
            return;
        TRACE("euf", j.display(tout << "merge: " << mk_bounded_pp(n1->get_expr(), m) << " == " << mk_bounded_pp(n2->get_expr(), m) << " ", m_display_justification) << "\n";);
        force_push();
        SASSERT(m_num_scopes == 0);
        ++m_stats.m_num_merge;
        if (r1->interpreted() && r2->interpreted()) {
            set_conflict(n1, n2, j);
            return;
        }
        if ((r1->class_size() > r2->class_size() && !r2->interpreted()) || r1->interpreted()) {
            std::swap(r1, r2);
            std::swap(n1, n2);
        }
        if ((m.is_true(r2->get_expr()) || m.is_false(r2->get_expr())) && j.is_congruence()) 
            add_literal(n1, false);        
        if (m.is_false(r2->get_expr()) && m.is_eq(n1->get_expr())) 
            new_diseq(n1);        
        for (enode* p : enode_parents(n1)) 
            m_table.erase(p);            
        for (enode* p : enode_parents(n2)) 
            m_table.erase(p);            
        push_eq(r1, n1, r2->num_parents());
        merge_justification(n1, n2, j);
        for (enode* c : enode_class(n1)) 
            c->m_root = r2;
        std::swap(r1->m_next, r2->m_next);
        r2->inc_class_size(r1->class_size());   
        r2->m_parents.append(r1->m_parents);
        merge_th_eq(r1, r2);
        m_worklist.push_back(r2);
    }

    void egraph::merge_th_eq(enode* n, enode* root) {
        SASSERT(n != root);
        for (auto iv : enode_th_vars(n)) {
            theory_id id = iv.get_id();
            theory_var v = root->get_th_var(id);
            if (v == null_theory_var) {                
                root->add_th_var(iv.get_var(), id, m_region);   
                m_updates.push_back(update_record(root, id, update_record::add_th_var()));
                add_th_diseqs(id, iv.get_var(), root);
            }
            else {
                SASSERT(v != iv.get_var());
                add_th_eq(id, v, iv.get_var(), n, root);
            }
        }
    }

    bool egraph::propagate() {
        SASSERT(m_new_lits_qhead <= m_new_lits.size());
        SASSERT(m_num_scopes == 0 || m_worklist.empty());
        unsigned head = 0, tail = m_worklist.size();
        while (head < tail && m.limit().inc() && !inconsistent()) {
            for (unsigned i = head; i < tail && !inconsistent(); ++i) {
                enode* n = m_worklist[i]->get_root();
                if (!n->is_marked1()) {
                    n->mark1();
                    m_worklist[i] = n;
                    reinsert(n);
                }
            }
            for (unsigned i = head; i < tail; ++i) 
                m_worklist[i]->unmark1();
            head = tail;
            tail = m_worklist.size();
        }
        m_worklist.reset();
        force_push();
        return 
            (m_new_lits_qhead < m_new_lits.size()) || 
            (m_new_th_eqs_qhead < m_new_th_eqs.size()) ||
            inconsistent();
    }

    void egraph::set_conflict(enode* n1, enode* n2, justification j) {
        ++m_stats.m_num_conflicts;
        if (m_inconsistent)
            return;
        m_inconsistent = true;
        m_updates.push_back(update_record(false, update_record::inconsistent()));
        m_n1 = n1;
        m_n2 = n2;
        m_justification = j;
    }

    void egraph::merge_justification(enode* n1, enode* n2, justification j) {
        SASSERT(!n1->get_root()->m_target);
        SASSERT(!n2->get_root()->m_target);
        SASSERT(n1->reaches(n1->get_root()));
        SASSERT(!n2->reaches(n1->get_root()));
        SASSERT(!n2->reaches(n1));
        n1->reverse_justification();
        n1->m_target = n2;
        n1->m_justification = j;
        SASSERT(n1->acyclic());
        SASSERT(n2->acyclic());
        SASSERT(n1->get_root()->reaches(n1));
        SASSERT(!n2->get_root()->m_target);
        TRACE("euf_verbose", tout << "merge " << n1->get_expr_id() << " " << n2->get_expr_id() << " updates: " << m_updates.size() << "\n";);
    }

    void egraph::unmerge_justification(enode* n1) {
        TRACE("euf_verbose", tout << "unmerge " << n1->get_expr_id() << " " << n1->m_target->get_expr_id() << "\n";);
        // r1 -> ..  -> n1 -> n2 -> ... -> r2
        // where n2 = n1->m_target
        SASSERT(n1->get_root()->reaches(n1));
        SASSERT(n1->m_target);
        n1->m_target        = nullptr;
        n1->m_justification = justification::axiom();
        n1->get_root()->reverse_justification();
        // ---------------
        // n1 -> ... -> r1
        // n2 -> ... -> r2
        SASSERT(n1->reaches(n1->get_root()));
        SASSERT(!n1->get_root()->m_target);
    }

    bool egraph::are_diseq(enode* a, enode* b) const {
        enode* ra = a->get_root(), * rb = b->get_root();
        if (ra == rb)
            return false;
        if (ra->interpreted() && rb->interpreted())
            return true;
        if (m.get_sort(ra->get_expr()) != m.get_sort(rb->get_expr()))
            return true;
        expr_ref eq(m.mk_eq(a->get_expr(), b->get_expr()), m);
        m_tmp_eq->m_args[0] = a;
        m_tmp_eq->m_args[1] = b;
        m_tmp_eq->m_expr = eq;
        SASSERT(m_tmp_eq->num_args() == 2);
        enode* r = m_table.find(m_tmp_eq);
        if (r && m_value(r->get_root()) == l_false)
            return true;
        return false;
    }

    /**
       \brief generate an explanation for a congruence.
       Each pair of children under a congruence have the same roots
       and therefore have a least common ancestor. We only need
       explanations up to the least common ancestors.
     */
    void egraph::push_congruence(enode* n1, enode* n2, bool comm) {
        SASSERT(is_app(n1->get_expr()));
        SASSERT(n1->get_decl() == n2->get_decl());
        if (m_used_cc && !comm) { 
            m_used_cc(to_app(n1->get_expr()), to_app(n2->get_expr()));
        }
        if (comm && 
            n1->get_arg(0)->get_root() == n2->get_arg(1)->get_root() &&
            n1->get_arg(1)->get_root() == n2->get_arg(0)->get_root()) {
            push_lca(n1->get_arg(0), n2->get_arg(1));
            push_lca(n1->get_arg(1), n2->get_arg(0));
            return;
        }
            
        for (unsigned i = 0; i < n1->num_args(); ++i) 
            push_lca(n1->get_arg(i), n2->get_arg(i));
    }

    enode* egraph::find_lca(enode* a, enode* b) {
        SASSERT(a->get_root() == b->get_root());
        a->mark2_targets<true>();
        while (!b->is_marked2()) 
            b = b->m_target;
        a->mark2_targets<false>();
        return b;
    }
    
    void egraph::push_to_lca(enode* n, enode* lca) {
        while (n != lca) {
            m_todo.push_back(n);
            n = n->m_target;
        }
    }

    void egraph::push_lca(enode* a, enode* b) {
        enode* lca = find_lca(a, b);
        push_to_lca(a, lca);
        push_to_lca(b, lca);
    }

    void egraph::push_todo(enode* n) {
        while (n) {
            m_todo.push_back(n);
            n = n->m_target;
        }
    }

    void egraph::begin_explain() {
        SASSERT(m_todo.empty());
    }

    void egraph::end_explain() {
        for (enode* n : m_todo) 
            n->unmark1();
        DEBUG_CODE(for (enode* n : m_nodes) SASSERT(!n->is_marked1()););
        m_todo.reset();        
    }

    template <typename T>
    void egraph::explain(ptr_vector<T>& justifications) {
        SASSERT(m_inconsistent);
        push_todo(m_n1);
        push_todo(m_n2);
        explain_eq(justifications, m_n1, m_n2, m_justification);
        explain_todo(justifications);
    }

    template <typename T>
    void egraph::explain_eq(ptr_vector<T>& justifications, enode* a, enode* b) {
        SASSERT(a->get_root() == b->get_root());
        
        enode* lca = find_lca(a, b);
        TRACE("euf_verbose", tout << "explain-eq: " << a->get_expr_id() << " = " << b->get_expr_id() 
            << ": " << mk_bounded_pp(a->get_expr(), m) 
            << " == " << mk_bounded_pp(b->get_expr(), m) 
            << " lca: " << mk_bounded_pp(lca->get_expr(), m) << "\n";);
        push_to_lca(a, lca);
        push_to_lca(b, lca);
        if (m_used_eq)
            m_used_eq(a->get_expr(), b->get_expr(), lca->get_expr());
        explain_todo(justifications);
    }

    template <typename T>
    void egraph::explain_todo(ptr_vector<T>& justifications) {
        for (unsigned i = 0; i < m_todo.size(); ++i) {
            enode* n = m_todo[i];
            if (n->m_target && !n->is_marked1()) {
                n->mark1();
                CTRACE("euf", m_display_justification, n->m_justification.display(tout << n->get_expr_id() << " = " << n->m_target->get_expr_id() << " ", m_display_justification) << "\n";);
                explain_eq(justifications, n, n->m_target, n->m_justification);
            }
        }
    }

    void egraph::invariant() {
        for (enode* n : m_nodes)
            n->invariant();
    }

    std::ostream& egraph::display(std::ostream& out, unsigned max_args, enode* n) const {
        out << n->get_expr_id() << " := ";
        expr* f = n->get_expr();
        if (is_app(f))
            out << mk_bounded_pp(f, m, 1) << " ";
        else if (is_quantifier(f))
            out << "q:" << f->get_id() << " ";
        else
            out << "v:" << f->get_id() << " ";
        if (!n->is_root()) 
            out << "[r " << n->get_root()->get_expr_id() << "] ";
        if (!n->m_parents.empty()) {
            out << "[p";
            for (enode* p : enode_parents(n))
                out << " " << p->get_expr_id();
            out << "] ";
        }
        if (n->has_th_vars()) {
            out << "[t";
            for (auto v : enode_th_vars(n))
                out << " " << v.get_id() << ":" << v.get_var();
            out << "] ";
        }
        if (n->m_target && m_display_justification)
            n->m_justification.display(out << "[j " << n->m_target->get_expr_id() << " ", m_display_justification) << "] ";
        out << "\n";
        return out;
    }

    std::ostream& egraph::display(std::ostream& out) const {
        out << "updates " << m_updates.size() << "\n";
        out << "newlits " << m_new_lits.size()   << " qhead: " << m_new_lits_qhead << "\n";
        out << "neweqs  " << m_new_th_eqs.size() << " qhead: " << m_new_th_eqs_qhead << "\n";
        m_table.display(out);
        unsigned max_args = 0;
        for (enode* n : m_nodes)
            max_args = std::max(max_args, n->num_args());
        for (enode* n : m_nodes) 
            display(out, max_args, n);          
        return out;
    }

    void egraph::collect_statistics(statistics& st) const {
        st.update("euf merge", m_stats.m_num_merge);
        st.update("euf conflicts", m_stats.m_num_conflicts);
        st.update("euf propagations eqs", m_stats.m_num_eqs);
        st.update("euf propagations theory eqs", m_stats.m_num_th_eqs);
        st.update("euf propagations theory diseqs", m_stats.m_num_th_diseqs);
        st.update("euf propagations literal", m_stats.m_num_lits);
    }

    void egraph::copy_from(egraph const& src, std::function<void*(void*)>& copy_justification) {
        SASSERT(m_scopes.empty());
        SASSERT(src.m_scopes.empty());
        SASSERT(m_nodes.empty());
        ptr_vector<enode> old_expr2new_enode, args;
        ast_translation tr(src.m, m);
        for (unsigned i = 0; i < src.m_nodes.size(); ++i) {
            enode* n1 = src.m_nodes[i];
            expr* e1 = src.m_exprs[i];
            SASSERT(!n1->has_th_vars());
            args.reset();
            for (unsigned j = 0; j < n1->num_args(); ++j) 
                args.push_back(old_expr2new_enode[n1->get_arg(j)->get_expr_id()]);
            expr*  e2 = tr(e1);
            enode* n2 = mk(e2, args.size(), args.c_ptr());
            old_expr2new_enode.setx(e1->get_id(), n2, nullptr);
        }
        for (unsigned i = 0; i < src.m_nodes.size(); ++i) {             
            enode* n1 = src.m_nodes[i];
            enode* n1t = n1->m_target;      
            enode* n2 = m_nodes[i];
            enode* n2t = n1t ? old_expr2new_enode[n1->get_expr_id()] : nullptr;
            SASSERT(!n1t || n2t);
            SASSERT(!n1t || src.m.get_sort(n1->get_expr()) == src.m.get_sort(n1t->get_expr()));
            SASSERT(!n1t || m.get_sort(n2->get_expr()) == m.get_sort(n2t->get_expr()));
            if (n1t && n2->get_root() != n2t->get_root()) 
                merge(n2, n2t, n1->m_justification.copy(copy_justification));
        }
        propagate();
    }
}

template void euf::egraph::explain(ptr_vector<int>& justifications);
template void euf::egraph::explain_todo(ptr_vector<int>& justifications);
template void euf::egraph::explain_eq(ptr_vector<int>& justifications, enode* a, enode* b);

template void euf::egraph::explain(ptr_vector<size_t>& justifications);
template void euf::egraph::explain_todo(ptr_vector<size_t>& justifications);
template void euf::egraph::explain_eq(ptr_vector<size_t>& justifications, enode* a, enode* b);

