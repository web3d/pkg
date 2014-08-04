/*-
 * Copyright (c) 2013 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/mount.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#define _WITH_GETLINE
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"
#include "private/pkgdb.h"
#include "private/pkg_jobs.h"

struct pkg_solve_item;

struct pkg_solve_variable {
	struct pkg_job_universe_item *unit;
	bool to_install;
	int priority;
	const char *digest;
	const char *uid;
	bool resolved;
	struct _pkg_solve_var_rule {
		struct pkg_solve_item *rule;
		struct _pkg_solve_var_rule *next;
	} *rules;
	int nrules;
	UT_hash_handle hh;
	struct pkg_solve_variable *next, *prev;
};

struct pkg_solve_item {
	int nitems;
	int nresolved;
	struct pkg_solve_variable *var;
	bool inverse;
	struct pkg_solve_item *next;
};

struct pkg_solve_rule {
	struct pkg_solve_item *items;
	struct pkg_solve_rule *next;
};

struct pkg_solve_problem {
	struct pkg_jobs *j;
	unsigned int rules_count;
	struct pkg_solve_rule *rules;
	struct pkg_solve_variable *variables_by_uid;
	struct pkg_solve_variable *variables;
	size_t nvars;
};

struct pkg_solve_impl_graph {
	struct pkg_solve_variable *var;
	struct pkg_solve_impl_graph *prev, *next;
};

/*
 * Use XOR here to implement the following logic:
 * atom is true if it is installed and not inverted or
 * if it is not installed but inverted
 */
#define PKG_SOLVE_CHECK_ITEM(item)				\
	((item)->var->to_install ^ (item)->inverse)

#define PKG_SOLVE_VAR_NEXT(a, e) ((e) == NULL ? &a[0] : (e + 1))

/**
 * Updates rules related to a single variable
 * @param var
 */
static void
pkg_solve_update_var_resolved (struct pkg_solve_variable *var)
{
	struct _pkg_solve_var_rule *rul;

	LL_FOREACH(var->rules, rul) {
		rul->rule->nresolved ++;
	}
}

static void
pkg_debug_print_rule (struct pkg_solve_item *rule)
{
	struct pkg_solve_item *it;
	struct sbuf *sb;
	int64_t expectlevel;

	/* Avoid expensive printing if debug level is less than required */
	expectlevel = pkg_object_int(pkg_config_get("DEBUG_LEVEL"));

	if (expectlevel < 2)
		return;

	sb = sbuf_new_auto();

	sbuf_printf(sb, "%s", "rule: (");

	LL_FOREACH(rule, it) {
		if (it->var->resolved) {
			sbuf_printf(sb, "%s%s%s(%c)%s", it->inverse ? "!" : "",
					it->var->uid,
					(it->var->unit->pkg->type == PKG_INSTALLED) ? "(l)" : "(r)",
					(it->var->to_install) ? '+' : '-',
					it->next ? " | " : ")");
		}
		else {
			sbuf_printf(sb, "%s%s%s%s", it->inverse ? "!" : "",
					it->var->uid,
					(it->var->unit->pkg->type == PKG_INSTALLED) ? "(l)" : "(r)",
					it->next ? " | " : ")");
		}
	}
	sbuf_finish(sb);
	pkg_debug(2, "%s", sbuf_data(sb));
	sbuf_delete(sb);
}

/**
 * Add variable to the implication graph
 * @param graph
 * @param var
 */
static void
pkg_solve_add_implication_graph(struct pkg_solve_impl_graph **graph,
	struct pkg_solve_variable *var)
{
	struct pkg_solve_impl_graph *it;

	it = malloc(sizeof(*it));
	if (it == NULL) {
		pkg_emit_errno("pkg_solve_add_implication_graph", "pkg_solve_impl_graph");
		return;
	}

	it->var = var;
	DL_APPEND(*graph, it);
}

/**
 * Propagate all units, must be called recursively
 * @param rules
 * @return true if there are units propagated
 */
static bool
pkg_solve_propagate_units(struct pkg_solve_problem *problem,
	int *propagated, struct pkg_solve_impl_graph **graph, bool top_level)
{
	struct pkg_solve_item *it, *unresolved = NULL;
	int solved_vars;
	struct _pkg_solve_var_rule *rul;
	struct pkg_solve_variable *var;
	bool ret;
	size_t i;

	do {
		solved_vars = 0;
		var = NULL;
		for (i = 0; i < problem->nvars; i ++) {
			var = &problem->variables[i];
check_again:
			/* Check for direct conflicts */
			LL_FOREACH(var->rules, rul) {
				unresolved = rul->rule;
				if (unresolved->nresolved == unresolved->nitems) {
					/* Check for direct conflict */
					ret = false;
					LL_FOREACH(unresolved, it) {
						if (it->var->resolved) {
							if (PKG_SOLVE_CHECK_ITEM(it)) {
								ret = true;
								break;
							}
						}
					}
					if (!ret) {
						if (top_level) {
							/*
							 * If it is top level propagation, then report that
							 * we have a conflicting clause there
							 */
							struct sbuf *err_msg = sbuf_new_auto();
							const char *pkg_name;
							int pkg_type;
							sbuf_printf(err_msg, "cannot resolve conflict between ");
							LL_FOREACH(unresolved, it) {
								pkg_get(it->var->unit->pkg, PKG_NAME, &pkg_name);
								pkg_type = it->var->unit->pkg->type;
								if (pkg_type == PKG_INSTALLED)
									sbuf_printf(err_msg, "local %s(want %s), ",
										pkg_name,
										it->var->to_install ? "keep" : "remove");
								else
									sbuf_printf(err_msg, "remote %s(want %s), ",
										pkg_name,
										it->var->to_install ? "install" : "ignore");
							}
							sbuf_finish(err_msg);
							pkg_emit_error("%splease resolve it manually", sbuf_data(err_msg));
							pkg_debug_print_rule(unresolved);
							sbuf_delete(err_msg);
						}
						return (false);
					}
				}
			}
			LL_FOREACH(var->rules, rul) {
				unresolved = rul->rule;
				if (unresolved->nresolved == unresolved->nitems - 1) {
					/* Check for unit */
					ret = false;
					LL_FOREACH(unresolved, it) {
						if (it->var->resolved) {
							if (PKG_SOLVE_CHECK_ITEM(it)) {
								ret = true;
								break;
							}
						}
					}
					if (!ret) {
						/* This is a unit */
						int resolved = 0;
						LL_FOREACH(unresolved, it) {
							if (!it->var->resolved) {
								it->var->to_install = (!it->inverse);
								it->var->resolved = true;
								pkg_solve_update_var_resolved(it->var);
								pkg_debug(2, "propagate %s-%s(%d) to %s",
										it->var->uid, it->var->digest,
										it->var->priority,
										it->var->to_install ? "install" : "delete");
								pkg_debug_print_rule(unresolved);
								resolved ++;
								break;
							}
						}
						if (resolved == 0) {
							pkg_debug_print_rule(unresolved);
							assert(resolved > 0);
						}
						solved_vars ++;
						(*propagated) ++;
						/* We want to try propagating all clauses for a single variable */
						if (graph != NULL)
							pkg_solve_add_implication_graph(graph, it->var);
						goto check_again;
					}
				}
			}
		}
	} while (solved_vars > 0);

	return (true);
}


/**
 * Propagate pure clauses
 */
static bool
pkg_solve_propagate_pure(struct pkg_solve_problem *problem)
{
	struct pkg_solve_variable *var;
	struct _pkg_solve_var_rule *rul;
	struct pkg_solve_item *it;
	size_t i;

	var = NULL;
	for (i = 0; i < problem->nvars; i ++) {
		var = &problem->variables[i];
		if (var->nrules == 0) {
			/* This variable is independent and should not change its state */
			assert (var->rules == NULL);
			var->to_install = (var->unit->pkg->type == PKG_INSTALLED);
			var->resolved = true;
			pkg_debug(2, "leave %s-%s(%d) to %s",
					var->uid, var->digest,
					var->priority, var->to_install ? "install" : "delete");
		}
		else {
			LL_FOREACH(var->rules, rul) {
				it = rul->rule;
				if (it->nitems == 1 && it->nresolved == 0) {
					it->var->to_install = (!it->inverse);
					it->var->resolved = true;
					pkg_debug(2, "requested %s-%s(%d) to %s",
							it->var->uid, it->var->digest,
							it->var->priority, it->var->to_install ? "install" : "delete");
					pkg_solve_update_var_resolved(it->var);
				}
			}
		}
	}

	return (true);
}

/*
 * Set initial guess based on a variable passed
 */
static bool
pkg_solve_initial_guess(struct pkg_solve_problem *problem,
		struct pkg_solve_variable *var)
{
	if (problem->j->type == PKG_JOBS_UPGRADE) {
		if (var->unit->pkg->type == PKG_INSTALLED) {
			/* For local packages assume true if we have no upgrade */
			if (var->unit->next == NULL && var->unit->prev == var->unit)
				return (true);
		}
		else {
			/* For remote packages we return true if they are upgrades for local ones */
			if (var->unit->next != NULL || var->unit->prev != var->unit)
				return (true);
		}
	}
	else {
		/* For all non-upgrade jobs be more conservative */
		if (var->unit->pkg->type == PKG_INSTALLED)
			return (true);
	}

	/* Otherwise set initial guess to false */
	return (false);
}

/*
 * For all variables in implication graph we reset their resolved values
 * Also we remove all variables propagated by this guess
 */
static void
pkg_solve_undo_guess(struct pkg_solve_impl_graph *graph)
{
	struct pkg_solve_impl_graph *cur, *tmp;

	DL_FOREACH_SAFE(graph, cur, tmp) {
		cur->var->resolved = false;
		if (cur != graph)
			free(cur);
	}

	graph->next = NULL;
	graph->prev = graph;
}

/**
 * Try to solve sat problem
 * @param rules incoming rules to solve
 * @param nrules number of rules
 * @param nvars number of variables
 * @return
 */
bool
pkg_solve_sat_problem(struct pkg_solve_problem *problem)
{
	int propagated;
	size_t i;
	struct pkg_solve_variable *var, *tvar;
	int64_t unresolved = 0, iters = 0;
	bool rc, backtrack = false, free_var;

	struct _solver_tree_elt {
		struct pkg_solve_variable *var;
		int guess;
		int inverses;
		struct pkg_solve_impl_graph *graph;
		struct _solver_tree_elt *prev, *next;
	} *solver_tree = NULL, *elt, *tmp;


	/* Obvious case */
	if (problem->rules_count == 0)
		return (true);

	/* Initially propagate explicit rules */
	pkg_solve_propagate_pure(problem);
	if (!pkg_solve_propagate_units(problem, &propagated, NULL, true)) {
		pkg_emit_error("SAT: conflicting request, cannot solve");
		return (false);
	}

	/* Set initial guess */
	elt = solver_tree;

	/* DPLL Algorithm */
	var = NULL;
	for (i = 0; i < problem->nvars; i ++) {
		var = &problem->variables[i];
		if (!var->resolved) {

			if (backtrack) {
				/* Shift var back */
				var = tvar;
				backtrack = false;
				i --;
			}

			if (elt == NULL) {
				/* Add new element to the backtracking queue */
				elt = malloc(sizeof (*elt));
				if (elt == NULL) {
					pkg_emit_errno("malloc", "_solver_tree_elt");
					LL_FREE(solver_tree, free);
					return (false);
				}
				elt->inverses = 0;
				elt->var = var;
				elt->guess = -1;
				elt->graph = NULL;
				DL_APPEND(solver_tree, elt);
			}
			assert (var == elt->var);

			if (elt->guess == -1) {
				var->to_install = pkg_solve_initial_guess(problem, var);
				var->resolved = true;
			}
			else {
				/* For analyzed variables we can only inverse previous guess */
				var->to_install = !elt->guess;
				var->resolved = true;
				elt->inverses ++;
			}

			unresolved ++;
			free_var = elt->guess == -1;
			pkg_debug(3, "setting guess for %s variable %s: %d(%d)",
				free_var ? "free" : "inversed", var->uid, var->to_install, elt->guess);
			/* Propagate units */
			pkg_solve_add_implication_graph(&elt->graph, var);

			if (!pkg_solve_propagate_units(problem, NULL, &elt->graph, false)) {
				pkg_solve_undo_guess(elt->graph);
				if (free_var) {
					/* This is free variable, so we can assign true or false to it */
					var->to_install = !var->to_install;
					rc = pkg_solve_propagate_units(problem, NULL, &elt->graph, false);
				}
				else {
					rc = false;
				}

				if (!rc) {
					/* Need to backtrack */
					iters ++;
					if (elt == NULL || elt->prev->next == NULL) {
						/* Cannot backtrack, UNSAT */
						pkg_debug(1, "problem is UNSAT problem after %d guesses", iters);
						LL_FREE(solver_tree, free);
						return (false);
					}
					pkg_debug(3, "backtrack from %s to %s", var->uid,
						elt->prev->var->uid);
					/* Set the current variable as free variable */
					elt->inverses = 0;
					free(elt->graph);
					elt->graph = NULL;
					elt->guess = -1;

					/* Go to the previous level */
					elt = elt->prev;
					tvar = elt->var;
					backtrack = true;
					continue;
				}
			}
			/* Assign the current guess */
			elt->guess = var->to_install;
			/* Move to the next elt */
			elt = elt->next;
		}
	}

	pkg_debug(1, "solved SAT problem in %d guesses", iters);

	DL_FOREACH_SAFE(solver_tree, elt, tmp) {
		LL_FREE(elt->graph, free);
		free(elt);
	}

	return (true);
}

/*
 * Utilities to convert jobs to SAT rule
 */

static struct pkg_solve_item *
pkg_solve_item_new(struct pkg_solve_variable *var)
{
	struct pkg_solve_item *result;

	result = calloc(1, sizeof(struct pkg_solve_item));

	if(result == NULL) {
		pkg_emit_errno("calloc", "pkg_solve_item");
		return (NULL);
	}

	result->var = var;
	result->inverse = false;

	return (result);
}

static struct pkg_solve_rule *
pkg_solve_rule_new(void)
{
	struct pkg_solve_rule *result;

	result = calloc(1, sizeof(struct pkg_solve_rule));

	if(result == NULL) {
		pkg_emit_errno("calloc", "pkg_solve_rule");
		return (NULL);
	}

	return (result);
}

static void
pkg_solve_variable_set(struct pkg_solve_variable *var,
	struct pkg_job_universe_item *item)
{
	const char *digest, *uid;

	var->unit = item;
	pkg_get(item->pkg, PKG_UNIQUEID, &uid, PKG_DIGEST, &digest);
	/* XXX: Is it safe to save a ptr here ? */
	var->digest = digest;
	var->uid = uid;
	var->prev = var;
}

static void
pkg_solve_rule_free(struct pkg_solve_rule *rule)
{
	struct pkg_solve_item *it, *tmp;

	LL_FOREACH_SAFE(rule->items, it, tmp) {
		free(it);
	}
	free(rule);
}


void
pkg_solve_problem_free(struct pkg_solve_problem *problem)
{
	struct pkg_solve_rule *r, *rtmp;
	struct pkg_solve_variable *v;
	struct _pkg_solve_var_rule *vrule, *vrtmp;

	LL_FOREACH_SAFE(problem->rules, r, rtmp) {
		pkg_solve_rule_free(r);
	}

	v = NULL;
	while((v = PKG_SOLVE_VAR_NEXT(problem->variables, v))) {
		HASH_DELETE(hh, problem->variables_by_uid, v);
		LL_FOREACH_SAFE(v->rules, vrule, vrtmp) {
			free(vrule);
		}
	}
	free(problem->variables);
}

static int
pkg_solve_add_var_rules (struct pkg_solve_variable *var,
		struct pkg_solve_item *rule, int nrules, bool iterate_vars,
		const char *desc)
{
	struct _pkg_solve_var_rule *head = NULL;
	struct pkg_solve_variable *tvar;

	LL_FOREACH(var, tvar) {
		pkg_debug(4, "solver: add %d-ary %s clause to variable %s-%s",
							nrules, desc, tvar->uid, tvar->digest);
		tvar->nrules += nrules;
		head = calloc(1, sizeof (struct _pkg_solve_var_rule));
		if (head == NULL) {
			pkg_emit_errno("calloc", "_pkg_solve_var_rule");
			return (EPKG_FATAL);
		}
		head->rule = rule;
		LL_PREPEND(tvar->rules, head);
		if (!iterate_vars)
			break;
	}
	pkg_debug_print_rule(head->rule);

	return (EPKG_OK);
}

#define RULE_ITEM_PREPEND(rule, item) do {									\
	(item)->nitems = (rule)->items ? (rule)->items->nitems + 1 : 1;			\
	LL_PREPEND((rule)->items, (item));										\
} while (0)

static int
pkg_solve_handle_provide (struct pkg_solve_problem *problem,
		struct pkg_job_provide *pr, struct pkg_solve_rule *rule, int *cnt)
{
	struct pkg_solve_item *it = NULL;
	const char *uid, *digest;
	struct pkg_solve_variable *var, *curvar;
	struct pkg_job_universe_item *un;

	/* Find the first package in the universe list */
	un = pr->un;
	while (un->prev->next != NULL) {
		un = un->prev;
	}

	/* Find the corresponding variables chain */
	pkg_get(un->pkg, PKG_DIGEST, &digest, PKG_UNIQUEID, &uid);
	HASH_FIND_STR(problem->variables_by_uid, uid, var);

	LL_FOREACH(var, curvar) {
		/* For each provide */
		it = pkg_solve_item_new(curvar);
		if (it == NULL)
			return (EPKG_FATAL);

		it->inverse = false;
		RULE_ITEM_PREPEND(rule, it);
		(*cnt) ++;
	}

	return (EPKG_OK);
}

static int
pkg_solve_add_depend_rule(struct pkg_solve_problem *problem,
		struct pkg_solve_variable *var,
		struct pkg_dep *dep)
{
	const char *uid;
	struct pkg_solve_variable *depvar, *curvar;
	struct pkg_solve_rule *rule = NULL;
	struct pkg_solve_item *it = NULL;
	int cnt;

	uid = dep->uid;
	HASH_FIND_STR(problem->variables_by_uid, uid, depvar);
	if (depvar == NULL) {
		pkg_debug(2, "cannot find variable dependency %s", uid);
		return (EPKG_END);
	}
	/* Dependency rule: (!A | B) */
	rule = pkg_solve_rule_new();
	if (rule == NULL)
		return (EPKG_FATAL);
	/* !A */
	it = pkg_solve_item_new(var);
	if (it == NULL)
		return (EPKG_FATAL);

	it->inverse = true;
	RULE_ITEM_PREPEND(rule, it);
	/* B1 | B2 | ... */
	cnt = 1;
	LL_FOREACH(depvar, curvar) {
		it = pkg_solve_item_new(curvar);
		if (it == NULL)
			return (EPKG_FATAL);

		it->inverse = false;
		RULE_ITEM_PREPEND(rule, it);
		cnt ++;
	}
	pkg_solve_add_var_rules (depvar, rule->items, cnt, true, "dependency");
	pkg_solve_add_var_rules (var, rule->items, cnt, false, "dependency");

	LL_PREPEND(problem->rules, rule);
	problem->rules_count ++;

	return (EPKG_OK);
}

static int
pkg_solve_add_conflict_rule(struct pkg_solve_problem *problem,
		struct pkg *pkg,
		struct pkg_solve_variable *var,
		struct pkg_conflict *conflict)
{
	const char *uid;
	struct pkg_solve_variable *confvar, *curvar;
	struct pkg_solve_rule *rule = NULL;
	struct pkg_solve_item *it = NULL;

	uid = pkg_conflict_uniqueid(conflict);
	HASH_FIND_STR(problem->variables_by_uid, uid, confvar);
	if (confvar == NULL) {
		pkg_debug(2, "cannot find conflict %s", uid);
		return (EPKG_END);
	}

	/* Add conflict rule from each of the alternative */
	LL_FOREACH(confvar, curvar) {
		if (conflict->type == PKG_CONFLICT_REMOTE_LOCAL) {
			/* Skip unappropriate packages */
			if (pkg->type == PKG_INSTALLED) {
				if (curvar->unit->pkg->type == PKG_INSTALLED)
					continue;
			}
			else {
				if (curvar->unit->pkg->type != PKG_INSTALLED)
					continue;
			}
		}
		else if (conflict->type == PKG_CONFLICT_REMOTE_REMOTE) {
			if (pkg->type == PKG_INSTALLED)
				continue;

			if (curvar->unit->pkg->type == PKG_INSTALLED)
				continue;
		}

		/* Conflict rule: (!A | !Bx) */
		rule = pkg_solve_rule_new();
		if (rule == NULL)
			return (EPKG_FATAL);
		/* !A */
		it = pkg_solve_item_new(var);
		if (it == NULL)
			return (EPKG_FATAL);

		it->inverse = true;
		RULE_ITEM_PREPEND(rule, it);
		/* !Bx */
		it = pkg_solve_item_new(curvar);
		if (it == NULL)
			return (EPKG_FATAL);

		it->inverse = true;
		RULE_ITEM_PREPEND(rule, it);

		LL_PREPEND(problem->rules, rule);
		problem->rules_count ++;
		pkg_solve_add_var_rules (curvar, rule->items, 2, false,
			"explicit conflict");
		pkg_solve_add_var_rules (var, rule->items, 2, false,
			"explicit conflict");
	}

	return (EPKG_OK);
}

static int
pkg_solve_add_require_rule(struct pkg_solve_problem *problem,
		struct pkg_solve_variable *var,
		struct pkg_shlib *shlib)
{
	struct pkg_solve_rule *rule;
	struct pkg_solve_item *it = NULL;
	struct pkg_job_provide *pr, *prhead;
	int cnt;

	HASH_FIND_STR(problem->j->universe->provides, pkg_shlib_name(shlib), prhead);
	if (prhead != NULL) {
		/* Require rule !A | P1 | P2 | P3 ... */
		rule = pkg_solve_rule_new();
		if (rule == NULL)
			return (EPKG_FATAL);
		/* !A */
		it = pkg_solve_item_new(var);
		if (it == NULL)
			return (EPKG_FATAL);

		it->inverse = true;
		RULE_ITEM_PREPEND(rule, it);
		/* B1 | B2 | ... */
		cnt = 1;
		LL_FOREACH(prhead, pr) {
			if (pkg_solve_handle_provide (problem, pr, rule,
				&cnt) != EPKG_OK)
				return (EPKG_FATAL);
		}

		if (cnt > 1) {
			pkg_solve_add_var_rules (var, rule->items, cnt, false, "provide");

			LL_PREPEND(problem->rules, rule);
			problem->rules_count ++;
		}
		else {
			/* Missing dependencies... */
			free(it);
			free(rule);
		}
	}
	else {
		/*
		 * XXX:
		 * This is terribly broken now so ignore till provides/requires
		 * are really fixed.
		 */
		pkg_debug(1, "solver: cannot find provide for required shlib %s",
			pkg_shlib_name(shlib));
	}

	return (EPKG_OK);
}

static int
pkg_solve_add_unary_rule(struct pkg_solve_problem *problem,
	struct pkg_solve_variable *var, bool inverse)
{
	struct pkg_solve_rule *rule;
	struct pkg_solve_item *it = NULL;

	pkg_debug(4, "solver: add variable from %s request with uid %s-%s",
		inverse ? "delete" : "install", var->uid, var->digest);

	it = pkg_solve_item_new(var);
	if (it == NULL)
		return (EPKG_FATAL);

	it->inverse = inverse;

	rule = pkg_solve_rule_new();
	if (rule == NULL)
		return (EPKG_FATAL);

	/* Requests are unary rules */
	RULE_ITEM_PREPEND(rule, it);
	pkg_solve_add_var_rules(var, it, 1, false, "unary request");
	LL_PREPEND(problem->rules, rule);
	problem->rules_count ++;

	return (EPKG_OK);
}

static int
pkg_solve_add_chain_rule(struct pkg_solve_problem *problem,
	struct pkg_solve_variable *var)
{
	struct pkg_solve_variable *curvar;
	struct pkg_solve_rule *rule;
	struct pkg_solve_item *it = NULL;

	LL_FOREACH(var->next, curvar) {
		/* Conflict rule: (!Ax | !Ay) */
		rule = pkg_solve_rule_new();
		if (rule == NULL)
			return (EPKG_FATAL);
		/* !Ax */
		it = pkg_solve_item_new(var);
		if (it == NULL)
			return (EPKG_FATAL);

		it->inverse = true;
		RULE_ITEM_PREPEND(rule, it);
		/* !Ay */
		it = pkg_solve_item_new(curvar);
		if (it == NULL)
			return (EPKG_FATAL);

		it->inverse = true;
		RULE_ITEM_PREPEND(rule, it);

		LL_PREPEND(problem->rules, rule);
		problem->rules_count ++;

		pkg_solve_add_var_rules (curvar, rule->items, 2, false, "chain conflict");
		pkg_solve_add_var_rules (var, rule->items, 2, false, "chain conflict");
	}

	return (EPKG_OK);
}

static int
pkg_solve_process_universe_variable(struct pkg_solve_problem *problem,
		struct pkg_solve_variable *var)
{
	struct pkg_dep *dep, *dtmp;
	struct pkg_conflict *conflict, *ctmp;
	struct pkg *pkg;
	struct pkg_solve_variable *cur_var;
	struct pkg_shlib *shlib = NULL;
	struct pkg_jobs *j = problem->j;
	struct pkg_job_request *jreq;
	bool chain_added = false;

	LL_FOREACH(var, cur_var) {
		pkg = cur_var->unit->pkg;

		/* Depends */
		HASH_ITER(hh, pkg->deps, dep, dtmp) {
			if (pkg_solve_add_depend_rule(problem, cur_var, dep) != EPKG_OK)
				continue;
		}

		/* Conflicts */
		HASH_ITER(hh, pkg->conflicts, conflict, ctmp) {
			if (pkg_solve_add_conflict_rule(problem, pkg, cur_var, conflict) !=
							EPKG_OK)
				continue;
		}

		/* Shlibs */
		shlib = NULL;
		if (pkg->type != PKG_INSTALLED) {
			while (pkg_shlibs_required(pkg, &shlib) == EPKG_OK) {
				if (pkg_solve_add_require_rule(problem, cur_var, shlib) != EPKG_OK)
					continue;
			}
		}

		/* Request */
		HASH_FIND_PTR(j->request_add, &cur_var->unit, jreq);
		if (jreq != NULL)
			pkg_solve_add_unary_rule(problem, cur_var, false);
		HASH_FIND_PTR(j->request_delete, &cur_var->unit, jreq);
		if (jreq != NULL)
			pkg_solve_add_unary_rule(problem, cur_var, true);

		/*
		 * If this var chain contains mutually conflicting vars
		 * we need to register conflicts with all following
		 * vars
		 */
		if (!chain_added && cur_var->next != NULL) {
			if (pkg_solve_add_chain_rule(problem, cur_var) != EPKG_OK)
				continue;

			chain_added = true;
		}
	}

	return (EPKG_OK);
err:
	return (EPKG_FATAL);
}

static int
pkg_solve_add_variable(struct pkg_job_universe_item *un,
		struct pkg_solve_problem *problem, size_t *n)
{
	struct pkg_job_universe_item *ucur;
	struct pkg_solve_variable *var = NULL, *tvar = NULL;
	const char *uid, *digest;

	LL_FOREACH(un, ucur) {
		assert(*n < problem->nvars);

		pkg_get(ucur->pkg, PKG_UNIQUEID, &uid, PKG_DIGEST, &digest);
		/* Add new variable */
		var = &problem->variables[*n];
		pkg_solve_variable_set(var, ucur);

		if (tvar == NULL) {
			pkg_debug(4, "solver: add variable from universe with uid %s", var->uid);
			HASH_ADD_KEYPTR(hh, problem->variables_by_uid,
				var->uid, strlen(var->uid), var);
			tvar = var;
		}
		else {
			/* Insert a variable to a chain */
			DL_APPEND(tvar, var);
		}
		(*n) ++;
	}

	return (EPKG_OK);
}

struct pkg_solve_problem *
pkg_solve_jobs_to_sat(struct pkg_jobs *j)
{
	struct pkg_solve_problem *problem;
	struct pkg_job_universe_item *un, *utmp;
	size_t i = 0;

	problem = calloc(1, sizeof(struct pkg_solve_problem));

	if (problem == NULL) {
		pkg_emit_errno("calloc", "pkg_solve_problem");
		return (NULL);
	}

	problem->j = j;
	problem->nvars = j->universe->nitems;
	problem->variables = calloc(problem->nvars, sizeof(struct pkg_solve_variable));

	if (problem->variables == NULL) {
		pkg_emit_errno("calloc", "variables");
		return (NULL);
	}

	/* Parse universe */
	HASH_ITER(hh, j->universe->items, un, utmp) {
		/* Add corresponding variables */
		if (pkg_solve_add_variable(un, problem, &i)
						== EPKG_FATAL)
			goto err;
	}

	/* Add rules for all conflict chains */
	HASH_ITER(hh, j->universe->items, un, utmp) {
		const char *uid;
		struct pkg_solve_variable *var;

		pkg_get(un->pkg, PKG_UNIQUEID, &uid);
		HASH_FIND_STR(problem->variables_by_uid, uid, var);
		if (var == NULL) {
			pkg_emit_error("internal solver error: variable %s is not found",
				uid);
			goto err;
		}
		if (pkg_solve_process_universe_variable(problem, var) != EPKG_OK)
			goto err;
	}

	if (problem->rules_count == 0) {
		pkg_debug(1, "problem has no requests");
		return (problem);
	}

	return (problem);

err:
	return (NULL);
}

struct pkg_solve_ordered_variable {
	struct pkg_solve_variable *var;
	int order;
	UT_hash_handle hh;
};

int
pkg_solve_dimacs_export(struct pkg_solve_problem *problem, FILE *f)
{
	struct pkg_solve_ordered_variable *ordered_variables = NULL, *nord;
	struct pkg_solve_variable *var;
	struct pkg_solve_rule *rule;
	struct pkg_solve_item *it;
	int cur_ord = 1;

	/* Order variables */
	var = NULL;
	while ((var = PKG_SOLVE_VAR_NEXT(problem->variables, var))) {
		nord = calloc(1, sizeof(struct pkg_solve_ordered_variable));
		nord->order = cur_ord ++;
		nord->var = var;
		HASH_ADD_PTR(ordered_variables, var, nord);
	}

	fprintf(f, "p cnf %d %d\n", (int)problem->nvars, problem->rules_count);

	LL_FOREACH(problem->rules, rule) {
		LL_FOREACH(rule->items, it) {
			HASH_FIND_PTR(ordered_variables, &it->var, nord);
			if (nord != NULL) {
				fprintf(f, "%s%d ", (it->inverse ? "-" : ""), nord->order);
			}
		}
		fprintf(f, "0\n");
	}

	HASH_FREE(ordered_variables, free);

	return (EPKG_OK);
}

static void
pkg_solve_insert_res_job (struct pkg_solve_variable *var,
		struct pkg_solve_problem *problem)
{
	struct pkg_solved *res;
	struct pkg_solve_variable *cur_var, *del_var = NULL, *add_var = NULL;
	int seen_add = 0, seen_del = 0;
	struct pkg_jobs *j = problem->j;

	LL_FOREACH(var, cur_var) {
		if (cur_var->to_install && cur_var->unit->pkg->type != PKG_INSTALLED) {
			add_var = cur_var;
			seen_add ++;
		}
		else if (!cur_var->to_install && cur_var->unit->pkg->type == PKG_INSTALLED) {
			del_var = cur_var;
			seen_del ++;
		}
	}
	if (seen_add > 1) {
		pkg_emit_error("internal solver error: more than two packages to install(%d) "
				"from the same uid: %s", seen_add, var->uid);
		return;
	}
	else if (seen_add != 0 || seen_del != 0) {
		if (seen_add > 0) {
			res = calloc(1, sizeof(struct pkg_solved));
			if (res == NULL) {
				pkg_emit_errno("calloc", "pkg_solved");
				return;
			}
			/* Pure install */
			if (seen_del == 0) {
				res->items[0] = add_var->unit;
				res->type = (j->type == PKG_JOBS_FETCH) ?
								PKG_SOLVED_FETCH : PKG_SOLVED_INSTALL;
				DL_APPEND(j->jobs, res);
				pkg_debug(3, "pkg_solve: schedule installation of %s %s",
					add_var->uid, add_var->digest);
			}
			else {
				/* Upgrade */
				res->items[0] = add_var->unit;
				res->items[1] = del_var->unit;
				res->type = PKG_SOLVED_UPGRADE;
				DL_APPEND(j->jobs, res);
				pkg_debug(3, "pkg_solve: schedule upgrade of %s from %s to %s",
					del_var->uid, del_var->digest, add_var->digest);
			}
			j->count ++;
		}

		/*
		 * For delete requests there could be multiple delete requests per UID,
		 * so we need to re-process vars and add all delete jobs required.
		 */
		LL_FOREACH(var, cur_var) {
			if (!cur_var->to_install && cur_var->unit->pkg->type == PKG_INSTALLED) {
				/* Skip already added items */
				if (seen_add > 0 && cur_var == del_var)
					continue;

				res = calloc(1, sizeof(struct pkg_solved));
				if (res == NULL) {
					pkg_emit_errno("calloc", "pkg_solved");
					return;
				}
				res->items[0] = cur_var->unit;
				res->type = PKG_SOLVED_DELETE;
				DL_APPEND(j->jobs, res);
				pkg_debug(3, "pkg_solve: schedule deletion of %s %s",
					cur_var->uid, cur_var->digest);
				j->count ++;
			}
		}
	}
	else {
		pkg_debug(2, "solver: ignoring package %s(%s) as its state has not been changed",
				var->uid, var->digest);
	}
}

int
pkg_solve_sat_to_jobs(struct pkg_solve_problem *problem)
{
	struct pkg_solve_variable *var, *tvar;

	HASH_ITER(hh, problem->variables_by_uid, var, tvar) {
		if (!var->resolved)
			return (EPKG_FATAL);

		pkg_debug(4, "solver: check variable with uid %s", var->uid);
		pkg_solve_insert_res_job(var, problem);
	}

	return (EPKG_OK);
}

int
pkg_solve_parse_sat_output(FILE *f, struct pkg_solve_problem *problem, struct pkg_jobs *j)
{
	struct pkg_solve_ordered_variable *ordered_variables = NULL, *nord;
	struct pkg_solve_variable *var;
	int cur_ord = 1, ret = EPKG_OK;
	char *line = NULL, *var_str, *begin;
	size_t linecap = 0;
	ssize_t linelen;
	bool got_sat = false, done = false;

	/* Order variables */
	var = NULL;
	while ((var = PKG_SOLVE_VAR_NEXT(problem->variables, var))) {
		nord = calloc(1, sizeof(struct pkg_solve_ordered_variable));
		nord->order = cur_ord ++;
		nord->var = var;
		HASH_ADD_INT(ordered_variables, order, nord);
	}

	while ((linelen = getline(&line, &linecap, f)) > 0) {
		if (strncmp(line, "SAT", 3) == 0) {
			got_sat = true;
		}
		else if (got_sat) {
			begin = line;
			do {
				var_str = strsep(&begin, " \t");
				/* Skip unexpected lines */
				if (var_str == NULL || (!isdigit(*var_str) && *var_str != '-'))
					continue;
				cur_ord = 0;
				cur_ord = abs(strtol(var_str, NULL, 10));
				if (cur_ord == 0) {
					done = true;
					break;
				}

				HASH_FIND_INT(ordered_variables, &cur_ord, nord);
				if (nord != NULL) {
					nord->var->resolved = true;
					nord->var->to_install = (*var_str != '-');
				}
			} while (begin != NULL);
		}
		else if (strncmp(line, "v ", 2) == 0) {
			begin = line + 2;
			do {
				var_str = strsep(&begin, " \t");
				/* Skip unexpected lines */
				if (var_str == NULL || (!isdigit(*var_str) && *var_str != '-'))
					continue;
				cur_ord = 0;
				cur_ord = abs(strtol(var_str, NULL, 10));
				if (cur_ord == 0) {
					done = true;
					break;
				}

				HASH_FIND_INT(ordered_variables, &cur_ord, nord);
				if (nord != NULL) {
					nord->var->resolved = true;
					nord->var->to_install = (*var_str != '-');
				}
			} while (begin != NULL);
		}
		else {
			/* Slightly ignore anything from solver */
			continue;
		}
	}

	if (done)
		ret = pkg_solve_sat_to_jobs(problem);
	else {
		pkg_emit_error("cannot parse sat solver output");
		ret = EPKG_FATAL;
	}

	HASH_FREE(ordered_variables, free);
	if (line != NULL)
		free(line);
	return (ret);
}
