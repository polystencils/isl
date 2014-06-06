/*
 * Copyright 2011      INRIA Saclay
 * Copyright 2012-2013 Ecole Normale Superieure
 *
 * Use of this software is governed by the MIT license
 *
 * Written by Sven Verdoolaege, INRIA Saclay - Ile-de-France,
 * Parc Club Orsay Universite, ZAC des vignes, 4 rue Jacques Monod,
 * 91893 Orsay, France
 * and Ecole Normale Superieure, 45 rue d'Ulm, 75230 Paris, France
 */

#include <isl_ctx_private.h>
#include <isl_map_private.h>
#include <isl_space_private.h>
#include <isl_aff_private.h>
#include <isl/hash.h>
#include <isl/constraint.h>
#include <isl/schedule.h>
#include <isl_mat_private.h>
#include <isl_vec_private.h>
#include <isl/set.h>
#include <isl_seq.h>
#include <isl_tab.h>
#include <isl_dim_map.h>
#include <isl/map_to_basic_set.h>
#include <isl_sort.h>
#include <isl_schedule_private.h>
#include <isl_band_private.h>
#include <isl_options_private.h>
#include <isl_tarjan.h>

/*
 * The scheduling algorithm implemented in this file was inspired by
 * Bondhugula et al., "Automatic Transformations for Communication-Minimized
 * Parallelization and Locality Optimization in the Polyhedral Model".
 */

 __isl_give isl_schedule_constraints *isl_schedule_constraints_copy(
	__isl_keep isl_schedule_constraints *sc)
{
	isl_ctx *ctx;
	isl_schedule_constraints *sc_copy;
	enum isl_edge_type i;

	ctx = isl_union_set_get_ctx(sc->domain);
	sc_copy = isl_calloc_type(ctx, struct isl_schedule_constraints);
	if (!sc_copy)
		return NULL;

	sc_copy->domain = isl_union_set_copy(sc->domain);
	if (!sc_copy->domain)
		return isl_schedule_constraints_free(sc_copy);

	for (i = isl_edge_first; i <= isl_edge_last; ++i) {
		sc_copy->constraint[i] = isl_union_map_copy(sc->constraint[i]);
		if (!sc_copy->constraint[i])
			return isl_schedule_constraints_free(sc_copy);
	}

	return sc_copy;
}


/* Construct an isl_schedule_constraints object for computing a schedule
 * on "domain".  The initial object does not impose any constraints.
 */
__isl_give isl_schedule_constraints *isl_schedule_constraints_on_domain(
	__isl_take isl_union_set *domain)
{
	isl_ctx *ctx;
	isl_space *space;
	isl_schedule_constraints *sc;
	isl_union_map *empty;
	enum isl_edge_type i;

	if (!domain)
		return NULL;

	ctx = isl_union_set_get_ctx(domain);
	sc = isl_calloc_type(ctx, struct isl_schedule_constraints);
	if (!sc)
		goto error;

	space = isl_union_set_get_space(domain);
	sc->domain = domain;
	empty = isl_union_map_empty(space);
	for (i = isl_edge_first; i <= isl_edge_last; ++i) {
		sc->constraint[i] = isl_union_map_copy(empty);
		if (!sc->constraint[i])
			sc->domain = isl_union_set_free(sc->domain);
	}
	isl_union_map_free(empty);

	if (!sc->domain)
		return isl_schedule_constraints_free(sc);

	return sc;
error:
	isl_union_set_free(domain);
	return NULL;
}

/* Replace the validity constraints of "sc" by "validity".
 */
__isl_give isl_schedule_constraints *isl_schedule_constraints_set_validity(
	__isl_take isl_schedule_constraints *sc,
	__isl_take isl_union_map *validity)
{
	if (!sc || !validity)
		goto error;

	isl_union_map_free(sc->constraint[isl_edge_validity]);
	sc->constraint[isl_edge_validity] = validity;

	return sc;
error:
	isl_schedule_constraints_free(sc);
	isl_union_map_free(validity);
	return NULL;
}

/* Replace the coincidence constraints of "sc" by "coincidence".
 */
__isl_give isl_schedule_constraints *isl_schedule_constraints_set_coincidence(
	__isl_take isl_schedule_constraints *sc,
	__isl_take isl_union_map *coincidence)
{
	if (!sc || !coincidence)
		goto error;

	isl_union_map_free(sc->constraint[isl_edge_coincidence]);
	sc->constraint[isl_edge_coincidence] = coincidence;

	return sc;
error:
	isl_schedule_constraints_free(sc);
	isl_union_map_free(coincidence);
	return NULL;
}

/* Replace the proximity constraints of "sc" by "proximity".
 */
__isl_give isl_schedule_constraints *isl_schedule_constraints_set_proximity(
	__isl_take isl_schedule_constraints *sc,
	__isl_take isl_union_map *proximity)
{
	if (!sc || !proximity)
		goto error;

	isl_union_map_free(sc->constraint[isl_edge_proximity]);
	sc->constraint[isl_edge_proximity] = proximity;

	return sc;
error:
	isl_schedule_constraints_free(sc);
	isl_union_map_free(proximity);
	return NULL;
}

/* Replace the conditional validity constraints of "sc" by "condition"
 * and "validity".
 */
__isl_give isl_schedule_constraints *
isl_schedule_constraints_set_conditional_validity(
	__isl_take isl_schedule_constraints *sc,
	__isl_take isl_union_map *condition,
	__isl_take isl_union_map *validity)
{
	if (!sc || !condition || !validity)
		goto error;

	isl_union_map_free(sc->constraint[isl_edge_condition]);
	sc->constraint[isl_edge_condition] = condition;
	isl_union_map_free(sc->constraint[isl_edge_conditional_validity]);
	sc->constraint[isl_edge_conditional_validity] = validity;

	return sc;
error:
	isl_schedule_constraints_free(sc);
	isl_union_map_free(condition);
	isl_union_map_free(validity);
	return NULL;
}

__isl_null isl_schedule_constraints *isl_schedule_constraints_free(
	__isl_take isl_schedule_constraints *sc)
{
	enum isl_edge_type i;

	if (!sc)
		return NULL;

	isl_union_set_free(sc->domain);
	for (i = isl_edge_first; i <= isl_edge_last; ++i)
		isl_union_map_free(sc->constraint[i]);

	free(sc);

	return NULL;
}

isl_ctx *isl_schedule_constraints_get_ctx(
	__isl_keep isl_schedule_constraints *sc)
{
	return sc ? isl_union_set_get_ctx(sc->domain) : NULL;
}

void isl_schedule_constraints_dump(__isl_keep isl_schedule_constraints *sc)
{
	if (!sc)
		return;

	fprintf(stderr, "domain: ");
	isl_union_set_dump(sc->domain);
	fprintf(stderr, "validity: ");
	isl_union_map_dump(sc->constraint[isl_edge_validity]);
	fprintf(stderr, "proximity: ");
	isl_union_map_dump(sc->constraint[isl_edge_proximity]);
	fprintf(stderr, "coincidence: ");
	isl_union_map_dump(sc->constraint[isl_edge_coincidence]);
	fprintf(stderr, "condition: ");
	isl_union_map_dump(sc->constraint[isl_edge_condition]);
	fprintf(stderr, "conditional_validity: ");
	isl_union_map_dump(sc->constraint[isl_edge_conditional_validity]);
}

/* Align the parameters of the fields of "sc".
 */
static __isl_give isl_schedule_constraints *
isl_schedule_constraints_align_params(__isl_take isl_schedule_constraints *sc)
{
	isl_space *space;
	enum isl_edge_type i;

	if (!sc)
		return NULL;

	space = isl_union_set_get_space(sc->domain);
	for (i = isl_edge_first; i <= isl_edge_last; ++i)
		space = isl_space_align_params(space,
				    isl_union_map_get_space(sc->constraint[i]));

	for (i = isl_edge_first; i <= isl_edge_last; ++i) {
		sc->constraint[i] = isl_union_map_align_params(
				    sc->constraint[i], isl_space_copy(space));
		if (!sc->constraint[i])
			space = isl_space_free(space);
	}
	sc->domain = isl_union_set_align_params(sc->domain, space);
	if (!sc->domain)
		return isl_schedule_constraints_free(sc);

	return sc;
}

/* Return the total number of isl_maps in the constraints of "sc".
 */
static __isl_give int isl_schedule_constraints_n_map(
	__isl_keep isl_schedule_constraints *sc)
{
	enum isl_edge_type i;
	int n = 0;

	for (i = isl_edge_first; i <= isl_edge_last; ++i)
		n += isl_union_map_n_map(sc->constraint[i]);

	return n;
}

/* Internal information about a node that is used during the construction
 * of a schedule.
 * dim represents the space in which the domain lives
 * sched is a matrix representation of the schedule being constructed
 *	for this node
 * sched_map is an isl_map representation of the same (partial) schedule
 *	sched_map may be NULL
 * rank is the number of linearly independent rows in the linear part
 *	of sched
 * the columns of cmap represent a change of basis for the schedule
 *	coefficients; the first rank columns span the linear part of
 *	the schedule rows
 * cinv is the inverse of cmap.
 * start is the first variable in the LP problem in the sequences that
 *	represents the schedule coefficients of this node
 * nvar is the dimension of the domain
 * nparam is the number of parameters or 0 if we are not constructing
 *	a parametric schedule
 *
 * scc is the index of SCC (or WCC) this node belongs to
 *
 * band contains the band index for each of the rows of the schedule.
 * band_id is used to differentiate between separate bands at the same
 * level within the same parent band, i.e., bands that are separated
 * by the parent band or bands that are independent of each other.
 * coincident contains a boolean for each of the rows of the schedule,
 * indicating whether the corresponding scheduling dimension satisfies
 * the coincidence constraints in the sense that the corresponding
 * dependence distances are zero.
 */
struct isl_sched_node {
	isl_space *dim;
	isl_mat *sched;
	isl_map *sched_map;
	int	 rank;
	isl_mat *cmap;
	isl_mat *cinv;
	int	 start;
	int	 nvar;
	int	 nparam;

	int	 scc;

	int	*band;
	int	*band_id;
	int	*coincident;
};

static int node_has_dim(const void *entry, const void *val)
{
	struct isl_sched_node *node = (struct isl_sched_node *)entry;
	isl_space *dim = (isl_space *)val;

	return isl_space_is_equal(node->dim, dim);
}

/* An edge in the dependence graph.  An edge may be used to
 * ensure validity of the generated schedule, to minimize the dependence
 * distance or both
 *
 * map is the dependence relation, with i -> j in the map if j depends on i
 * tagged_condition and tagged_validity contain the union of all tagged
 *	condition or conditional validity dependence relations that
 *	specialize the dependence relation "map"; that is,
 *	if (i -> a) -> (j -> b) is an element of "tagged_condition"
 *	or "tagged_validity", then i -> j is an element of "map".
 *	If these fields are NULL, then they represent the empty relation.
 * src is the source node
 * dst is the sink node
 * validity is set if the edge is used to ensure correctness
 * coincidence is used to enforce zero dependence distances
 * proximity is set if the edge is used to minimize dependence distances
 * condition is set if the edge represents a condition
 *	for a conditional validity schedule constraint
 * local can only be set for condition edges and indicates that
 *	the dependence distance over the edge should be zero
 * conditional_validity is set if the edge is used to conditionally
 *	ensure correctness
 *
 * For validity edges, start and end mark the sequence of inequality
 * constraints in the LP problem that encode the validity constraint
 * corresponding to this edge.
 */
struct isl_sched_edge {
	isl_map *map;
	isl_union_map *tagged_condition;
	isl_union_map *tagged_validity;

	struct isl_sched_node *src;
	struct isl_sched_node *dst;

	unsigned validity : 1;
	unsigned coincidence : 1;
	unsigned proximity : 1;
	unsigned local : 1;
	unsigned condition : 1;
	unsigned conditional_validity : 1;

	int start;
	int end;
};

/* Internal information about the dependence graph used during
 * the construction of the schedule.
 *
 * intra_hmap is a cache, mapping dependence relations to their dual,
 *	for dependences from a node to itself
 * inter_hmap is a cache, mapping dependence relations to their dual,
 *	for dependences between distinct nodes
 *
 * n is the number of nodes
 * node is the list of nodes
 * maxvar is the maximal number of variables over all nodes
 * max_row is the allocated number of rows in the schedule
 * n_row is the current (maximal) number of linearly independent
 *	rows in the node schedules
 * n_total_row is the current number of rows in the node schedules
 * n_band is the current number of completed bands
 * band_start is the starting row in the node schedules of the current band
 * root is set if this graph is the original dependence graph,
 *	without any splitting
 *
 * sorted contains a list of node indices sorted according to the
 *	SCC to which a node belongs
 *
 * n_edge is the number of edges
 * edge is the list of edges
 * max_edge contains the maximal number of edges of each type;
 *	in particular, it contains the number of edges in the inital graph.
 * edge_table contains pointers into the edge array, hashed on the source
 *	and sink spaces; there is one such table for each type;
 *	a given edge may be referenced from more than one table
 *	if the corresponding relation appears in more than of the
 *	sets of dependences
 *
 * node_table contains pointers into the node array, hashed on the space
 *
 * region contains a list of variable sequences that should be non-trivial
 *
 * lp contains the (I)LP problem used to obtain new schedule rows
 *
 * src_scc and dst_scc are the source and sink SCCs of an edge with
 *	conflicting constraints
 *
 * scc represents the number of components
 */
struct isl_sched_graph {
	isl_map_to_basic_set *intra_hmap;
	isl_map_to_basic_set *inter_hmap;

	struct isl_sched_node *node;
	int n;
	int maxvar;
	int max_row;
	int n_row;

	int *sorted;

	int n_band;
	int n_total_row;
	int band_start;

	int root;

	struct isl_sched_edge *edge;
	int n_edge;
	int max_edge[isl_edge_last + 1];
	struct isl_hash_table *edge_table[isl_edge_last + 1];

	struct isl_hash_table *node_table;
	struct isl_region *region;

	isl_basic_set *lp;

	int src_scc;
	int dst_scc;

	int scc;
};

/* Initialize node_table based on the list of nodes.
 */
static int graph_init_table(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i;

	graph->node_table = isl_hash_table_alloc(ctx, graph->n);
	if (!graph->node_table)
		return -1;

	for (i = 0; i < graph->n; ++i) {
		struct isl_hash_table_entry *entry;
		uint32_t hash;

		hash = isl_space_get_hash(graph->node[i].dim);
		entry = isl_hash_table_find(ctx, graph->node_table, hash,
					    &node_has_dim,
					    graph->node[i].dim, 1);
		if (!entry)
			return -1;
		entry->data = &graph->node[i];
	}

	return 0;
}

/* Return a pointer to the node that lives within the given space,
 * or NULL if there is no such node.
 */
static struct isl_sched_node *graph_find_node(isl_ctx *ctx,
	struct isl_sched_graph *graph, __isl_keep isl_space *dim)
{
	struct isl_hash_table_entry *entry;
	uint32_t hash;

	hash = isl_space_get_hash(dim);
	entry = isl_hash_table_find(ctx, graph->node_table, hash,
				    &node_has_dim, dim, 0);

	return entry ? entry->data : NULL;
}

static int edge_has_src_and_dst(const void *entry, const void *val)
{
	const struct isl_sched_edge *edge = entry;
	const struct isl_sched_edge *temp = val;

	return edge->src == temp->src && edge->dst == temp->dst;
}

/* Add the given edge to graph->edge_table[type].
 */
static int graph_edge_table_add(isl_ctx *ctx, struct isl_sched_graph *graph,
	enum isl_edge_type type, struct isl_sched_edge *edge)
{
	struct isl_hash_table_entry *entry;
	uint32_t hash;

	hash = isl_hash_init();
	hash = isl_hash_builtin(hash, edge->src);
	hash = isl_hash_builtin(hash, edge->dst);
	entry = isl_hash_table_find(ctx, graph->edge_table[type], hash,
				    &edge_has_src_and_dst, edge, 1);
	if (!entry)
		return -1;
	entry->data = edge;

	return 0;
}

/* Allocate the edge_tables based on the maximal number of edges of
 * each type.
 */
static int graph_init_edge_tables(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i;

	for (i = 0; i <= isl_edge_last; ++i) {
		graph->edge_table[i] = isl_hash_table_alloc(ctx,
							    graph->max_edge[i]);
		if (!graph->edge_table[i])
			return -1;
	}

	return 0;
}

/* If graph->edge_table[type] contains an edge from the given source
 * to the given destination, then return the hash table entry of this edge.
 * Otherwise, return NULL.
 */
static struct isl_hash_table_entry *graph_find_edge_entry(
	struct isl_sched_graph *graph,
	enum isl_edge_type type,
	struct isl_sched_node *src, struct isl_sched_node *dst)
{
	isl_ctx *ctx = isl_space_get_ctx(src->dim);
	uint32_t hash;
	struct isl_sched_edge temp = { .src = src, .dst = dst };

	hash = isl_hash_init();
	hash = isl_hash_builtin(hash, temp.src);
	hash = isl_hash_builtin(hash, temp.dst);
	return isl_hash_table_find(ctx, graph->edge_table[type], hash,
				    &edge_has_src_and_dst, &temp, 0);
}


/* If graph->edge_table[type] contains an edge from the given source
 * to the given destination, then return this edge.
 * Otherwise, return NULL.
 */
static struct isl_sched_edge *graph_find_edge(struct isl_sched_graph *graph,
	enum isl_edge_type type,
	struct isl_sched_node *src, struct isl_sched_node *dst)
{
	struct isl_hash_table_entry *entry;

	entry = graph_find_edge_entry(graph, type, src, dst);
	if (!entry)
		return NULL;

	return entry->data;
}

/* Check whether the dependence graph has an edge of the given type
 * between the given two nodes.
 */
static int graph_has_edge(struct isl_sched_graph *graph,
	enum isl_edge_type type,
	struct isl_sched_node *src, struct isl_sched_node *dst)
{
	struct isl_sched_edge *edge;
	int empty;

	edge = graph_find_edge(graph, type, src, dst);
	if (!edge)
		return 0;

	empty = isl_map_plain_is_empty(edge->map);
	if (empty < 0)
		return -1;

	return !empty;
}

/* Look for any edge with the same src, dst and map fields as "model".
 *
 * Return the matching edge if one can be found.
 * Return "model" if no matching edge is found.
 * Return NULL on error.
 */
static struct isl_sched_edge *graph_find_matching_edge(
	struct isl_sched_graph *graph, struct isl_sched_edge *model)
{
	enum isl_edge_type i;
	struct isl_sched_edge *edge;

	for (i = isl_edge_first; i <= isl_edge_last; ++i) {
		int is_equal;

		edge = graph_find_edge(graph, i, model->src, model->dst);
		if (!edge)
			continue;
		is_equal = isl_map_plain_is_equal(model->map, edge->map);
		if (is_equal < 0)
			return NULL;
		if (is_equal)
			return edge;
	}

	return model;
}

/* Remove the given edge from all the edge_tables that refer to it.
 */
static void graph_remove_edge(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge)
{
	isl_ctx *ctx = isl_map_get_ctx(edge->map);
	enum isl_edge_type i;

	for (i = isl_edge_first; i <= isl_edge_last; ++i) {
		struct isl_hash_table_entry *entry;

		entry = graph_find_edge_entry(graph, i, edge->src, edge->dst);
		if (!entry)
			continue;
		if (entry->data != edge)
			continue;
		isl_hash_table_remove(ctx, graph->edge_table[i], entry);
	}
}

/* Check whether the dependence graph has any edge
 * between the given two nodes.
 */
static int graph_has_any_edge(struct isl_sched_graph *graph,
	struct isl_sched_node *src, struct isl_sched_node *dst)
{
	enum isl_edge_type i;
	int r;

	for (i = isl_edge_first; i <= isl_edge_last; ++i) {
		r = graph_has_edge(graph, i, src, dst);
		if (r < 0 || r)
			return r;
	}

	return r;
}

/* Check whether the dependence graph has a validity edge
 * between the given two nodes.
 *
 * Conditional validity edges are essentially validity edges that
 * can be ignored if the corresponding condition edges are iteration private.
 * Here, we are only checking for the presence of validity
 * edges, so we need to consider the conditional validity edges too.
 * In particular, this function is used during the detection
 * of strongly connected components and we cannot ignore
 * conditional validity edges during this detection.
 */
static int graph_has_validity_edge(struct isl_sched_graph *graph,
	struct isl_sched_node *src, struct isl_sched_node *dst)
{
	int r;

	r = graph_has_edge(graph, isl_edge_validity, src, dst);
	if (r < 0 || r)
		return r;

	return graph_has_edge(graph, isl_edge_conditional_validity, src, dst);
}

static int graph_alloc(isl_ctx *ctx, struct isl_sched_graph *graph,
	int n_node, int n_edge)
{
	int i;

	graph->n = n_node;
	graph->n_edge = n_edge;
	graph->node = isl_calloc_array(ctx, struct isl_sched_node, graph->n);
	graph->sorted = isl_calloc_array(ctx, int, graph->n);
	graph->region = isl_alloc_array(ctx, struct isl_region, graph->n);
	graph->edge = isl_calloc_array(ctx,
					struct isl_sched_edge, graph->n_edge);

	graph->intra_hmap = isl_map_to_basic_set_alloc(ctx, 2 * n_edge);
	graph->inter_hmap = isl_map_to_basic_set_alloc(ctx, 2 * n_edge);

	if (!graph->node || !graph->region || (graph->n_edge && !graph->edge) ||
	    !graph->sorted)
		return -1;

	for(i = 0; i < graph->n; ++i)
		graph->sorted[i] = i;

	return 0;
}

static void graph_free(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i;

	isl_map_to_basic_set_free(graph->intra_hmap);
	isl_map_to_basic_set_free(graph->inter_hmap);
 
	if (graph->node)
		for (i = 0; i < graph->n; ++i) {
			isl_space_free(graph->node[i].dim);
			isl_mat_free(graph->node[i].sched);
			isl_map_free(graph->node[i].sched_map);
			isl_mat_free(graph->node[i].cmap);
			isl_mat_free(graph->node[i].cinv);
			if (graph->root) {
				free(graph->node[i].band);
				free(graph->node[i].band_id);
				free(graph->node[i].zero);
			}
 		}
 	free(graph->node);
 	free(graph->sorted);
	if (graph->edge)
		for (i = 0; i < graph->n_edge; ++i) {
			isl_map_free(graph->edge[i].map);
			isl_union_map_free(graph->edge[i].tagged_condition);
			isl_union_map_free(graph->edge[i].tagged_validity);
		}
 	free(graph->edge);
 	free(graph->region);
	for (i = 0; i <= isl_edge_last; ++i)
		isl_hash_table_free(ctx, graph->edge_table[i]);
	isl_hash_table_free(ctx, graph->node_table);
	isl_basic_set_free(graph->lp);
}

/* For each "set" on which this function is called, increment
 * graph->n by one and update graph->maxvar.
 */
static int init_n_maxvar(__isl_take isl_set *set, void *user)
{
	struct isl_sched_graph *graph = user;
	int nvar = isl_set_dim(set, isl_dim_set);

	graph->n++;
	if (nvar > graph->maxvar)
		graph->maxvar = nvar;

	isl_set_free(set);

	return 0;
}

/* Compute the number of rows that should be allocated for the schedule.
 * The graph can be split at most "n - 1" times, there can be at most
 * two rows for each dimension in the iteration domains (in particular,
 * we usually have one row, but it may be split by split_scaled),
 * and there can be one extra row for ordering the statements.
 * Note that if we have actually split "n - 1" times, then no ordering
 * is needed, so in principle we could use "graph->n + 2 * graph->maxvar - 1".
 */
static int compute_max_row(struct isl_sched_graph *graph,
	__isl_keep isl_union_set *domain)
{
	graph->n = 0;
	graph->maxvar = 0;
	if (isl_union_set_foreach_set(domain, &init_n_maxvar, graph) < 0)
		return -1;
	graph->max_row = graph->n + 2 * graph->maxvar;

	return 0;
}

/* Add a new node to the graph representing the given set.
 */
static int extract_node(__isl_take isl_set *set, void *user)
{
	int nvar, nparam;
	isl_ctx *ctx;
	isl_space *space;
	isl_mat *sched;
	struct isl_sched_graph *graph = user;
	int *band, *band_id, *coincident;

	ctx = isl_set_get_ctx(set);
	space = isl_set_get_space(set);
	isl_set_free(set);
	nvar = isl_space_dim(space, isl_dim_set);
	nparam = isl_space_dim(space, isl_dim_param);
	if (!ctx->opt->schedule_parametric)
		nparam = 0;
	sched = isl_mat_alloc(ctx, 0, 1 + nparam + nvar);
	graph->node[graph->n].dim = space;
	graph->node[graph->n].nvar = nvar;
	graph->node[graph->n].nparam = nparam;
	graph->node[graph->n].sched = sched;
	graph->node[graph->n].sched_map = NULL;
	band = isl_alloc_array(ctx, int, graph->max_row);
	graph->node[graph->n].band = band;
	band_id = isl_calloc_array(ctx, int, graph->max_row);
	graph->node[graph->n].band_id = band_id;
	coincident = isl_calloc_array(ctx, int, graph->max_row);
	graph->node[graph->n].coincident = coincident;
	graph->n++;

	if (!space || !sched ||
	    (graph->max_row && (!band || !band_id || !coincident)))
		return -1;

	return 0;
}

struct isl_extract_edge_data {
	enum isl_edge_type type;
	struct isl_sched_graph *graph;
};

/* Merge edge2 into edge1, freeing the contents of edge2.
 * "type" is the type of the schedule constraint from which edge2 was
 * extracted.
 * Return 0 on success and -1 on failure.
 *
 * edge1 and edge2 are assumed to have the same value for the map field.
 */
static int merge_edge(enum isl_edge_type type, struct isl_sched_edge *edge1,
	struct isl_sched_edge *edge2)
{
	edge1->validity |= edge2->validity;
	edge1->coincidence |= edge2->coincidence;
	edge1->proximity |= edge2->proximity;
	edge1->condition |= edge2->condition;
	edge1->conditional_validity |= edge2->conditional_validity;
	isl_map_free(edge2->map);

	if (type == isl_edge_condition) {
		if (!edge1->tagged_condition)
			edge1->tagged_condition = edge2->tagged_condition;
		else
			edge1->tagged_condition =
				isl_union_map_union(edge1->tagged_condition,
						    edge2->tagged_condition);
	}

	if (type == isl_edge_conditional_validity) {
		if (!edge1->tagged_validity)
			edge1->tagged_validity = edge2->tagged_validity;
		else
			edge1->tagged_validity =
				isl_union_map_union(edge1->tagged_validity,
						    edge2->tagged_validity);
	}

	if (type == isl_edge_condition && !edge1->tagged_condition)
		return -1;
	if (type == isl_edge_conditional_validity && !edge1->tagged_validity)
		return -1;

	return 0;
}

/* Insert dummy tags in domain and range of "map".
 *
 * In particular, if "map" is of the form
 *
 *	A -> B
 *
 * then return
 *
 *	[A -> dummy_tag] -> [B -> dummy_tag]
 *
 * where the dummy_tags are identical and equal to any dummy tags
 * introduced by any other call to this function.
 */
static __isl_give isl_map *insert_dummy_tags(__isl_take isl_map *map)
{
	static char dummy;
	isl_ctx *ctx;
	isl_id *id;
	isl_space *space;
	isl_set *domain, *range;

	ctx = isl_map_get_ctx(map);

	id = isl_id_alloc(ctx, NULL, &dummy);
	space = isl_space_params(isl_map_get_space(map));
	space = isl_space_set_from_params(space);
	space = isl_space_set_tuple_id(space, isl_dim_set, id);
	space = isl_space_map_from_set(space);

	domain = isl_map_wrap(map);
	range = isl_map_wrap(isl_map_universe(space));
	map = isl_map_from_domain_and_range(domain, range);
	map = isl_map_zip(map);

	return map;
}

/* Add a new edge to the graph based on the given map
 * and add it to data->graph->edge_table[data->type].
 * If a dependence relation of a given type happens to be identical
 * to one of the dependence relations of a type that was added before,
 * then we don't create a new edge, but instead mark the original edge
 * as also representing a dependence of the current type.
 *
 * Edges of type isl_edge_condition or isl_edge_conditional_validity
 * may be specified as "tagged" dependence relations.  That is, "map"
 * may contain elements (i -> a) -> (j -> b), where i -> j denotes
 * the dependence on iterations and a and b are tags.
 * edge->map is set to the relation containing the elements i -> j,
 * while edge->tagged_condition and edge->tagged_validity contain
 * the union of all the "map" relations
 * for which extract_edge is called that result in the same edge->map.
 */
static int extract_edge(__isl_take isl_map *map, void *user)
{
	isl_ctx *ctx = isl_map_get_ctx(map);
	struct isl_extract_edge_data *data = user;
	struct isl_sched_graph *graph = data->graph;
	struct isl_sched_node *src, *dst;
	isl_space *dim;
	struct isl_sched_edge *edge;
	isl_map *tagged = NULL;

	if (data->type == isl_edge_condition ||
	    data->type == isl_edge_conditional_validity) {
		if (isl_map_can_zip(map)) {
			tagged = isl_map_copy(map);
			map = isl_set_unwrap(isl_map_domain(isl_map_zip(map)));
		} else {
			tagged = insert_dummy_tags(isl_map_copy(map));
		}
	}

	dim = isl_space_domain(isl_map_get_space(map));
	src = graph_find_node(ctx, graph, dim);
	isl_space_free(dim);
	dim = isl_space_range(isl_map_get_space(map));
	dst = graph_find_node(ctx, graph, dim);
	isl_space_free(dim);

	if (!src || !dst) {
		isl_map_free(map);
		isl_map_free(tagged);
		return 0;
	}

	graph->edge[graph->n_edge].src = src;
	graph->edge[graph->n_edge].dst = dst;
	graph->edge[graph->n_edge].map = map;
	graph->edge[graph->n_edge].validity = 0;
	graph->edge[graph->n_edge].coincidence = 0;
	graph->edge[graph->n_edge].proximity = 0;
	graph->edge[graph->n_edge].condition = 0;
	graph->edge[graph->n_edge].local = 0;
	graph->edge[graph->n_edge].conditional_validity = 0;
	graph->edge[graph->n_edge].tagged_condition = NULL;
	graph->edge[graph->n_edge].tagged_validity = NULL;
	if (data->type == isl_edge_validity)
		graph->edge[graph->n_edge].validity = 1;
	if (data->type == isl_edge_coincidence)
		graph->edge[graph->n_edge].coincidence = 1;
	if (data->type == isl_edge_proximity)
		graph->edge[graph->n_edge].proximity = 1;
	if (data->type == isl_edge_condition) {
		graph->edge[graph->n_edge].condition = 1;
		graph->edge[graph->n_edge].tagged_condition =
					isl_union_map_from_map(tagged);
	}
	if (data->type == isl_edge_conditional_validity) {
		graph->edge[graph->n_edge].conditional_validity = 1;
		graph->edge[graph->n_edge].tagged_validity =
					isl_union_map_from_map(tagged);
	}

	edge = graph_find_matching_edge(graph, &graph->edge[graph->n_edge]);
	if (!edge) {
		graph->n_edge++;
		return -1;
	}
	if (edge == &graph->edge[graph->n_edge])
		return graph_edge_table_add(ctx, graph, data->type,
				    &graph->edge[graph->n_edge++]);

	if (merge_edge(data->type, edge, &graph->edge[graph->n_edge]) < 0)
		return -1;

	return graph_edge_table_add(ctx, graph, data->type, edge);
}

/* Check whether there is any dependence from node[j] to node[i]
 * or from node[i] to node[j].
 */
static int node_follows_weak(int i, int j, void *user)
{
	int f;
	struct isl_sched_graph *graph = user;

	f = graph_has_any_edge(graph, &graph->node[j], &graph->node[i]);
	if (f < 0 || f)
		return f;
	return graph_has_any_edge(graph, &graph->node[i], &graph->node[j]);
}

/* Check whether there is a (conditional) validity dependence from node[j]
 * to node[i], forcing node[i] to follow node[j].
 */
static int node_follows_strong(int i, int j, void *user)
{
	struct isl_sched_graph *graph = user;

	return graph_has_validity_edge(graph, &graph->node[j], &graph->node[i]);
}

/* Use Tarjan's algorithm for computing the strongly connected components
 * in the dependence graph (only validity edges).
 * If weak is set, we consider the graph to be undirected and
 * we effectively compute the (weakly) connected components.
 * Additionally, we also consider other edges when weak is set.
 */
static int detect_ccs(isl_ctx *ctx, struct isl_sched_graph *graph, int weak)
{
	int i, n;
	struct isl_tarjan_graph *g = NULL;

	g = isl_tarjan_graph_init(ctx, graph->n,
		weak ? &node_follows_weak : &node_follows_strong, graph);
	if (!g)
		return -1;

	graph->scc = 0;
	i = 0;
	n = graph->n;
	while (n) {
		while (g->order[i] != -1) {
			graph->node[g->order[i]].scc = graph->scc;
			--n;
			++i;
		}
		++i;
		graph->scc++;
	}

	isl_tarjan_graph_free(g);

	return 0;
}

/* Apply Tarjan's algorithm to detect the strongly connected components
 * in the dependence graph.
 */
static int detect_sccs(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	return detect_ccs(ctx, graph, 0);
}

/* Apply Tarjan's algorithm to detect the (weakly) connected components
 * in the dependence graph.
 */
static int detect_wccs(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	return detect_ccs(ctx, graph, 1);
}

static int cmp_scc(const void *a, const void *b, void *data)
{
	struct isl_sched_graph *graph = data;
	const int *i1 = a;
	const int *i2 = b;

	return graph->node[*i1].scc - graph->node[*i2].scc;
}

/* Sort the elements of graph->sorted according to the corresponding SCCs.
 */
static int sort_sccs(struct isl_sched_graph *graph)
{
	return isl_sort(graph->sorted, graph->n, sizeof(int), &cmp_scc, graph);
}

/* Given a dependence relation R from a node to itself,
 * construct the set of coefficients of valid constraints for elements
 * in that dependence relation.
 * In particular, the result contains tuples of coefficients
 * c_0, c_n, c_x such that
 *
 *	c_0 + c_n n + c_x y - c_x x >= 0 for each (x,y) in R
 *
 * or, equivalently,
 *
 *	c_0 + c_n n + c_x d >= 0 for each d in delta R = { y - x | (x,y) in R }
 *
 * We choose here to compute the dual of delta R.
 * Alternatively, we could have computed the dual of R, resulting
 * in a set of tuples c_0, c_n, c_x, c_y, and then
 * plugged in (c_0, c_n, c_x, -c_x).
 */
static __isl_give isl_basic_set *intra_coefficients(
	struct isl_sched_graph *graph, __isl_take isl_map *map)
{
	isl_set *delta;
	isl_basic_set *coef;

	if (isl_map_to_basic_set_has(graph->intra_hmap, map))
		return isl_map_to_basic_set_get(graph->intra_hmap, map);

	delta = isl_set_remove_divs(isl_map_deltas(isl_map_copy(map)));
	coef = isl_set_coefficients(delta);
	graph->intra_hmap = isl_map_to_basic_set_set(graph->intra_hmap, map,
					isl_basic_set_copy(coef));

	return coef;
}

/* Given a dependence relation R, construct the set of coefficients
 * of valid constraints for elements in that dependence relation.
 * In particular, the result contains tuples of coefficients
 * c_0, c_n, c_x, c_y such that
 *
 *	c_0 + c_n n + c_x x + c_y y >= 0 for each (x,y) in R
 *
 */
static __isl_give isl_basic_set *inter_coefficients(
	struct isl_sched_graph *graph, __isl_take isl_map *map)
{
	isl_set *set;
	isl_basic_set *coef;

	if (isl_map_to_basic_set_has(graph->inter_hmap, map))
		return isl_map_to_basic_set_get(graph->inter_hmap, map);

	set = isl_map_wrap(isl_map_remove_divs(isl_map_copy(map)));
	coef = isl_set_coefficients(set);
	graph->inter_hmap = isl_map_to_basic_set_set(graph->inter_hmap, map,
					isl_basic_set_copy(coef));

	return coef;
}

/* Add constraints to graph->lp that force validity for the given
 * dependence from a node i to itself.
 * That is, add constraints that enforce
 *
 *	(c_i_0 + c_i_n n + c_i_x y) - (c_i_0 + c_i_n n + c_i_x x)
 *	= c_i_x (y - x) >= 0
 *
 * for each (x,y) in R.
 * We obtain general constraints on coefficients (c_0, c_n, c_x)
 * of valid constraints for (y - x) and then plug in (0, 0, c_i_x^+ - c_i_x^-),
 * where c_i_x = c_i_x^+ - c_i_x^-, with c_i_x^+ and c_i_x^- non-negative.
 * In graph->lp, the c_i_x^- appear before their c_i_x^+ counterpart.
 *
 * Actually, we do not construct constraints for the c_i_x themselves,
 * but for the coefficients of c_i_x written as a linear combination
 * of the columns in node->cmap.
 */
static int add_intra_validity_constraints(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge)
{
	unsigned total;
	isl_map *map = isl_map_copy(edge->map);
	isl_ctx *ctx = isl_map_get_ctx(map);
	isl_space *dim;
	isl_dim_map *dim_map;
	isl_basic_set *coef;
	struct isl_sched_node *node = edge->src;

	coef = intra_coefficients(graph, map);

	dim = isl_space_domain(isl_space_unwrap(isl_basic_set_get_space(coef)));

	coef = isl_basic_set_transform_dims(coef, isl_dim_set,
		    isl_space_dim(dim, isl_dim_set), isl_mat_copy(node->cmap));
	if (!coef)
		goto error;

	total = isl_basic_set_total_dim(graph->lp);
	dim_map = isl_dim_map_alloc(ctx, total);
	isl_dim_map_range(dim_map, node->start + 2 * node->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  node->nvar, -1);
	isl_dim_map_range(dim_map, node->start + 2 * node->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  node->nvar, 1);
	graph->lp = isl_basic_set_extend_constraints(graph->lp,
			coef->n_eq, coef->n_ineq);
	graph->lp = isl_basic_set_add_constraints_dim_map(graph->lp,
							   coef, dim_map);
	isl_space_free(dim);

	return 0;
error:
	isl_space_free(dim);
	return -1;
}

/* Add constraints to graph->lp that force validity for the given
 * dependence from node i to node j.
 * That is, add constraints that enforce
 *
 *	(c_j_0 + c_j_n n + c_j_x y) - (c_i_0 + c_i_n n + c_i_x x) >= 0
 *
 * for each (x,y) in R.
 * We obtain general constraints on coefficients (c_0, c_n, c_x, c_y)
 * of valid constraints for R and then plug in
 * (c_j_0 - c_i_0, c_j_n^+ - c_j_n^- - (c_i_n^+ - c_i_n^-),
 *  c_j_x^+ - c_j_x^- - (c_i_x^+ - c_i_x^-)),
 * where c_* = c_*^+ - c_*^-, with c_*^+ and c_*^- non-negative.
 * In graph->lp, the c_*^- appear before their c_*^+ counterpart.
 *
 * Actually, we do not construct constraints for the c_*_x themselves,
 * but for the coefficients of c_*_x written as a linear combination
 * of the columns in node->cmap.
 */
static int add_inter_validity_constraints(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge)
{
	unsigned total;
	isl_map *map = isl_map_copy(edge->map);
	isl_ctx *ctx = isl_map_get_ctx(map);
	isl_space *dim;
	isl_dim_map *dim_map;
	isl_basic_set *coef;
	struct isl_sched_node *src = edge->src;
	struct isl_sched_node *dst = edge->dst;

	coef = inter_coefficients(graph, map);

	dim = isl_space_domain(isl_space_unwrap(isl_basic_set_get_space(coef)));

	coef = isl_basic_set_transform_dims(coef, isl_dim_set,
		    isl_space_dim(dim, isl_dim_set), isl_mat_copy(src->cmap));
	coef = isl_basic_set_transform_dims(coef, isl_dim_set,
		    isl_space_dim(dim, isl_dim_set) + src->nvar,
		    isl_mat_copy(dst->cmap));
	if (!coef)
		goto error;

	total = isl_basic_set_total_dim(graph->lp);
	dim_map = isl_dim_map_alloc(ctx, total);

	isl_dim_map_range(dim_map, dst->start, 0, 0, 0, 1, 1);
	isl_dim_map_range(dim_map, dst->start + 1, 2, 1, 1, dst->nparam, -1);
	isl_dim_map_range(dim_map, dst->start + 2, 2, 1, 1, dst->nparam, 1);
	isl_dim_map_range(dim_map, dst->start + 2 * dst->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set) + src->nvar, 1,
			  dst->nvar, -1);
	isl_dim_map_range(dim_map, dst->start + 2 * dst->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set) + src->nvar, 1,
			  dst->nvar, 1);

	isl_dim_map_range(dim_map, src->start, 0, 0, 0, 1, -1);
	isl_dim_map_range(dim_map, src->start + 1, 2, 1, 1, src->nparam, 1);
	isl_dim_map_range(dim_map, src->start + 2, 2, 1, 1, src->nparam, -1);
	isl_dim_map_range(dim_map, src->start + 2 * src->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  src->nvar, 1);
	isl_dim_map_range(dim_map, src->start + 2 * src->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  src->nvar, -1);

	edge->start = graph->lp->n_ineq;
	graph->lp = isl_basic_set_extend_constraints(graph->lp,
			coef->n_eq, coef->n_ineq);
	graph->lp = isl_basic_set_add_constraints_dim_map(graph->lp,
							   coef, dim_map);
	if (!graph->lp)
		goto error;
	isl_space_free(dim);
	edge->end = graph->lp->n_ineq;

	return 0;
error:
	isl_space_free(dim);
	return -1;
}

/* Add constraints to graph->lp that bound the dependence distance for the given
 * dependence from a node i to itself.
 * If s = 1, we add the constraint
 *
 *	c_i_x (y - x) <= m_0 + m_n n
 *
 * or
 *
 *	-c_i_x (y - x) + m_0 + m_n n >= 0
 *
 * for each (x,y) in R.
 * If s = -1, we add the constraint
 *
 *	-c_i_x (y - x) <= m_0 + m_n n
 *
 * or
 *
 *	c_i_x (y - x) + m_0 + m_n n >= 0
 *
 * for each (x,y) in R.
 * We obtain general constraints on coefficients (c_0, c_n, c_x)
 * of valid constraints for (y - x) and then plug in (m_0, m_n, -s * c_i_x),
 * with each coefficient (except m_0) represented as a pair of non-negative
 * coefficients.
 *
 * Actually, we do not construct constraints for the c_i_x themselves,
 * but for the coefficients of c_i_x written as a linear combination
 * of the columns in node->cmap.
 *
 *
 * If "local" is set, then we add constraints
 *
 *	c_i_x (y - x) <= 0
 *
 * or
 *
 *	-c_i_x (y - x) <= 0
 *
 * instead, forcing the dependence distance to be (less than or) equal to 0.
 * That is, we plug in (0, 0, -s * c_i_x),
 * Note that dependences marked local are treated as validity constraints
 * by add_all_validity_constraints and therefore also have
 * their distances bounded by 0 from below.
 */
static int add_intra_proximity_constraints(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge, int s, int local)
{
	unsigned total;
	unsigned nparam;
	isl_map *map = isl_map_copy(edge->map);
	isl_ctx *ctx = isl_map_get_ctx(map);
	isl_space *dim;
	isl_dim_map *dim_map;
	isl_basic_set *coef;
	struct isl_sched_node *node = edge->src;

	coef = intra_coefficients(graph, map);

	dim = isl_space_domain(isl_space_unwrap(isl_basic_set_get_space(coef)));

	coef = isl_basic_set_transform_dims(coef, isl_dim_set,
		    isl_space_dim(dim, isl_dim_set), isl_mat_copy(node->cmap));
	if (!coef)
		goto error;

	nparam = isl_space_dim(node->dim, isl_dim_param);
	total = isl_basic_set_total_dim(graph->lp);
	dim_map = isl_dim_map_alloc(ctx, total);

	if (!local) {
		isl_dim_map_range(dim_map, 1, 0, 0, 0, 1, 1);
		isl_dim_map_range(dim_map, 4, 2, 1, 1, nparam, -1);
		isl_dim_map_range(dim_map, 5, 2, 1, 1, nparam, 1);
	}
	isl_dim_map_range(dim_map, node->start + 2 * node->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  node->nvar, s);
	isl_dim_map_range(dim_map, node->start + 2 * node->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  node->nvar, -s);
	graph->lp = isl_basic_set_extend_constraints(graph->lp,
			coef->n_eq, coef->n_ineq);
	graph->lp = isl_basic_set_add_constraints_dim_map(graph->lp,
							   coef, dim_map);
	isl_space_free(dim);

	return 0;
error:
	isl_space_free(dim);
	return -1;
}

/* Add constraints to graph->lp that bound the dependence distance for the given
 * dependence from node i to node j.
 * If s = 1, we add the constraint
 *
 *	(c_j_0 + c_j_n n + c_j_x y) - (c_i_0 + c_i_n n + c_i_x x)
 *		<= m_0 + m_n n
 *
 * or
 *
 *	-(c_j_0 + c_j_n n + c_j_x y) + (c_i_0 + c_i_n n + c_i_x x) +
 *		m_0 + m_n n >= 0
 *
 * for each (x,y) in R.
 * If s = -1, we add the constraint
 *
 *	-((c_j_0 + c_j_n n + c_j_x y) - (c_i_0 + c_i_n n + c_i_x x))
 *		<= m_0 + m_n n
 *
 * or
 *
 *	(c_j_0 + c_j_n n + c_j_x y) - (c_i_0 + c_i_n n + c_i_x x) +
 *		m_0 + m_n n >= 0
 *
 * for each (x,y) in R.
 * We obtain general constraints on coefficients (c_0, c_n, c_x, c_y)
 * of valid constraints for R and then plug in
 * (m_0 - s*c_j_0 + s*c_i_0, m_n - s*c_j_n + s*c_i_n,
 *  -s*c_j_x+s*c_i_x)
 * with each coefficient (except m_0, c_j_0 and c_i_0)
 * represented as a pair of non-negative coefficients.
 *
 * Actually, we do not construct constraints for the c_*_x themselves,
 * but for the coefficients of c_*_x written as a linear combination
 * of the columns in node->cmap.
 *
 *
 * If "local" is set, then we add constraints
 *
 *	(c_j_0 + c_j_n n + c_j_x y) - (c_i_0 + c_i_n n + c_i_x x) <= 0
 *
 * or
 *
 *	-((c_j_0 + c_j_n n + c_j_x y) - (c_i_0 + c_i_n n + c_i_x x)) <= 0
 *
 * instead, forcing the dependence distance to be (less than or) equal to 0.
 * That is, we plug in
 * (-s*c_j_0 + s*c_i_0, -s*c_j_n + s*c_i_n, -s*c_j_x+s*c_i_x).
 * Note that dependences marked local are treated as validity constraints
 * by add_all_validity_constraints and therefore also have
 * their distances bounded by 0 from below.
 */
static int add_inter_proximity_constraints(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge, int s, int local)
{
	unsigned total;
	unsigned nparam;
	isl_map *map = isl_map_copy(edge->map);
	isl_ctx *ctx = isl_map_get_ctx(map);
	isl_space *dim;
	isl_dim_map *dim_map;
	isl_basic_set *coef;
	struct isl_sched_node *src = edge->src;
	struct isl_sched_node *dst = edge->dst;

	coef = inter_coefficients(graph, map);

	dim = isl_space_domain(isl_space_unwrap(isl_basic_set_get_space(coef)));

	coef = isl_basic_set_transform_dims(coef, isl_dim_set,
		    isl_space_dim(dim, isl_dim_set), isl_mat_copy(src->cmap));
	coef = isl_basic_set_transform_dims(coef, isl_dim_set,
		    isl_space_dim(dim, isl_dim_set) + src->nvar,
		    isl_mat_copy(dst->cmap));
	if (!coef)
		goto error;

	nparam = isl_space_dim(src->dim, isl_dim_param);
	total = isl_basic_set_total_dim(graph->lp);
	dim_map = isl_dim_map_alloc(ctx, total);

	if (!local) {
		isl_dim_map_range(dim_map, 1, 0, 0, 0, 1, 1);
		isl_dim_map_range(dim_map, 4, 2, 1, 1, nparam, -1);
		isl_dim_map_range(dim_map, 5, 2, 1, 1, nparam, 1);
	}

	isl_dim_map_range(dim_map, dst->start, 0, 0, 0, 1, -s);
	isl_dim_map_range(dim_map, dst->start + 1, 2, 1, 1, dst->nparam, s);
	isl_dim_map_range(dim_map, dst->start + 2, 2, 1, 1, dst->nparam, -s);
	isl_dim_map_range(dim_map, dst->start + 2 * dst->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set) + src->nvar, 1,
			  dst->nvar, s);
	isl_dim_map_range(dim_map, dst->start + 2 * dst->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set) + src->nvar, 1,
			  dst->nvar, -s);

	isl_dim_map_range(dim_map, src->start, 0, 0, 0, 1, s);
	isl_dim_map_range(dim_map, src->start + 1, 2, 1, 1, src->nparam, -s);
	isl_dim_map_range(dim_map, src->start + 2, 2, 1, 1, src->nparam, s);
	isl_dim_map_range(dim_map, src->start + 2 * src->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  src->nvar, -s);
	isl_dim_map_range(dim_map, src->start + 2 * src->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  src->nvar, s);

	graph->lp = isl_basic_set_extend_constraints(graph->lp,
			coef->n_eq, coef->n_ineq);
	graph->lp = isl_basic_set_add_constraints_dim_map(graph->lp,
							   coef, dim_map);
	isl_space_free(dim);

	return 0;
error:
	isl_space_free(dim);
	return -1;
}

/* Add all validity constraints to graph->lp.
 *
 * An edge that is forced to be local needs to have its dependence
 * distances equal to zero.  We take care of bounding them by 0 from below
 * here.  add_all_proximity_constraints takes care of bounding them by 0
 * from above.
 *
 * If "use_coincidence" is set, then we treat coincidence edges as local edges.
 * Otherwise, we ignore them.
 */
static int add_all_validity_constraints(struct isl_sched_graph *graph,
	int use_coincidence)
{
	int i;

	for (i = 0; i < graph->n_edge; ++i) {
		struct isl_sched_edge *edge= &graph->edge[i];
		int local;

		local = edge->local || (edge->coincidence && use_coincidence);
		if (!edge->validity && !local)
			continue;
		if (edge->src != edge->dst)
			continue;
		if (add_intra_validity_constraints(graph, edge) < 0)
			return -1;
	}

	for (i = 0; i < graph->n_edge; ++i) {
		struct isl_sched_edge *edge = &graph->edge[i];
		int local;

		local = edge->local || (edge->coincidence && use_coincidence);
		if (!edge->validity && !local)
			continue;
		if (edge->src == edge->dst)
			continue;
		if (add_inter_validity_constraints(graph, edge) < 0)
			return -1;
	}

	return 0;
}

/* Add constraints to graph->lp that bound the dependence distance
 * for all dependence relations.
 * If a given proximity dependence is identical to a validity
 * dependence, then the dependence distance is already bounded
 * from below (by zero), so we only need to bound the distance
 * from above.  (This includes the case of "local" dependences
 * which are treated as validity dependence by add_all_validity_constraints.)
 * Otherwise, we need to bound the distance both from above and from below.
 *
 * If "use_coincidence" is set, then we treat coincidence edges as local edges.
 * Otherwise, we ignore them.
 */
static int add_all_proximity_constraints(struct isl_sched_graph *graph,
	int use_coincidence)
{
	int i;

	for (i = 0; i < graph->n_edge; ++i) {
		struct isl_sched_edge *edge= &graph->edge[i];
		int local;

		local = edge->local || (edge->coincidence && use_coincidence);
		if (!edge->proximity && !local)
			continue;
		if (edge->src == edge->dst &&
		    add_intra_proximity_constraints(graph, edge, 1, local) < 0)
			return -1;
		if (edge->src != edge->dst &&
		    add_inter_proximity_constraints(graph, edge, 1, local) < 0)
			return -1;
		if (edge->validity || local)
			continue;
		if (edge->src == edge->dst &&
		    add_intra_proximity_constraints(graph, edge, -1, 0) < 0)
			return -1;
		if (edge->src != edge->dst &&
		    add_inter_proximity_constraints(graph, edge, -1, 0) < 0)
			return -1;
	}

	return 0;
}

/* Compute a basis for the rows in the linear part of the schedule
 * and extend this basis to a full basis.  The remaining rows
 * can then be used to force linear independence from the rows
 * in the schedule.
 *
 * In particular, given the schedule rows S, we compute
 *
 *	S   = H Q
 *	S U = H
 *
 * with H the Hermite normal form of S.  That is, all but the
 * first rank columns of H are zero and so each row in S is
 * a linear combination of the first rank rows of Q.
 * The matrix Q is then transposed because we will write the
 * coefficients of the next schedule row as a column vector s
 * and express this s as a linear combination s = Q c of the
 * computed basis.
 * Similarly, the matrix U is transposed such that we can
 * compute the coefficients c = U s from a schedule row s.
 */
static int node_update_cmap(struct isl_sched_node *node)
{
	isl_mat *H, *U, *Q;
	int n_row = isl_mat_rows(node->sched);

	H = isl_mat_sub_alloc(node->sched, 0, n_row,
			      1 + node->nparam, node->nvar);

	H = isl_mat_left_hermite(H, 0, &U, &Q);
	isl_mat_free(node->cmap);
	isl_mat_free(node->cinv);
	node->cmap = isl_mat_transpose(Q);
	node->cinv = isl_mat_transpose(U);
	node->rank = isl_mat_initial_non_zero_cols(H);
	isl_mat_free(H);

	if (!node->cmap || !node->cinv || node->rank < 0)
		return -1;
	return 0;
}

/* How many times should we count the constraints in "edge"?
 *
 * If carry is set, then we are counting the number of
 * (validity or conditional validity) constraints that will be added
 * in setup_carry_lp and we count each edge exactly once.
 *
 * Otherwise, we count as follows
 * validity		-> 1 (>= 0)
 * validity+proximity	-> 2 (>= 0 and upper bound)
 * proximity		-> 2 (lower and upper bound)
 * local(+any)		-> 2 (>= 0 and <= 0)
 *
 * If an edge is only marked conditional_validity then it counts
 * as zero since it is only checked afterwards.
 *
 * If "use_coincidence" is set, then we treat coincidence edges as local edges.
 * Otherwise, we ignore them.
 */
static int edge_multiplicity(struct isl_sched_edge *edge, int carry,
	int use_coincidence)
{
	if (carry && !edge->validity && !edge->conditional_validity)
		return 0;
	if (carry)
		return 1;
	if (edge->proximity || edge->local)
		return 2;
	if (use_coincidence && edge->coincidence)
		return 2;
	if (edge->validity)
		return 1;
	return 0;
}

/* Count the number of equality and inequality constraints
 * that will be added for the given map.
 *
 * "use_coincidence" is set if we should take into account coincidence edges.
 */
static int count_map_constraints(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge, __isl_take isl_map *map,
	int *n_eq, int *n_ineq, int carry, int use_coincidence)
{
	isl_basic_set *coef;
	int f = edge_multiplicity(edge, carry, use_coincidence);

	if (f == 0) {
		isl_map_free(map);
		return 0;
	}

	if (edge->src == edge->dst)
		coef = intra_coefficients(graph, map);
	else
		coef = inter_coefficients(graph, map);
	if (!coef)
		return -1;
	*n_eq += f * coef->n_eq;
	*n_ineq += f * coef->n_ineq;
	isl_basic_set_free(coef);

	return 0;
}

/* Count the number of equality and inequality constraints
 * that will be added to the main lp problem.
 * We count as follows
 * validity		-> 1 (>= 0)
 * validity+proximity	-> 2 (>= 0 and upper bound)
 * proximity		-> 2 (lower and upper bound)
 * local(+any)		-> 2 (>= 0 and <= 0)
 *
 * If "use_coincidence" is set, then we treat coincidence edges as local edges.
 * Otherwise, we ignore them.
 */
static int count_constraints(struct isl_sched_graph *graph,
	int *n_eq, int *n_ineq, int use_coincidence)
{
	int i;

	*n_eq = *n_ineq = 0;
	for (i = 0; i < graph->n_edge; ++i) {
		struct isl_sched_edge *edge= &graph->edge[i];
		isl_map *map = isl_map_copy(edge->map);

		if (count_map_constraints(graph, edge, map, n_eq, n_ineq,
					    0, use_coincidence) < 0)
			return -1;
	}

	return 0;
}

/* Count the number of constraints that will be added by
 * add_bound_coefficient_constraints and increment *n_eq and *n_ineq
 * accordingly.
 *
 * In practice, add_bound_coefficient_constraints only adds inequalities.
 */
static int count_bound_coefficient_constraints(isl_ctx *ctx,
	struct isl_sched_graph *graph, int *n_eq, int *n_ineq)
{
	int i;

	if (ctx->opt->schedule_max_coefficient == -1)
		return 0;

	for (i = 0; i < graph->n; ++i)
		*n_ineq += 2 * graph->node[i].nparam + 2 * graph->node[i].nvar;

	return 0;
}

/* Add constraints that bound the values of the variable and parameter
 * coefficients of the schedule.
 *
 * The maximal value of the coefficients is defined by the option
 * 'schedule_max_coefficient'.
 */
static int add_bound_coefficient_constraints(isl_ctx *ctx,
	struct isl_sched_graph *graph)
{
	int i, j, k;
	int max_coefficient;
	int total;

	max_coefficient = ctx->opt->schedule_max_coefficient;

	if (max_coefficient == -1)
		return 0;

	total = isl_basic_set_total_dim(graph->lp);

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		for (j = 0; j < 2 * node->nparam + 2 * node->nvar; ++j) {
			int dim;
			k = isl_basic_set_alloc_inequality(graph->lp);
			if (k < 0)
				return -1;
			dim = 1 + node->start + 1 + j;
			isl_seq_clr(graph->lp->ineq[k], 1 +  total);
			isl_int_set_si(graph->lp->ineq[k][dim], -1);
			isl_int_set_si(graph->lp->ineq[k][0], max_coefficient);
		}
	}

	return 0;
}

/* Construct an ILP problem for finding schedule coefficients
 * that result in non-negative, but small dependence distances
 * over all dependences.
 * In particular, the dependence distances over proximity edges
 * are bounded by m_0 + m_n n and we compute schedule coefficients
 * with small values (preferably zero) of m_n and m_0.
 *
 * All variables of the ILP are non-negative.  The actual coefficients
 * may be negative, so each coefficient is represented as the difference
 * of two non-negative variables.  The negative part always appears
 * immediately before the positive part.
 * Other than that, the variables have the following order
 *
 *	- sum of positive and negative parts of m_n coefficients
 *	- m_0
 *	- sum of positive and negative parts of all c_n coefficients
 *		(unconstrained when computing non-parametric schedules)
 *	- sum of positive and negative parts of all c_x coefficients
 *	- positive and negative parts of m_n coefficients
 *	- for each node
 *		- c_i_0
 *		- positive and negative parts of c_i_n (if parametric)
 *		- positive and negative parts of c_i_x
 *
 * The c_i_x are not represented directly, but through the columns of
 * node->cmap.  That is, the computed values are for variable t_i_x
 * such that c_i_x = Q t_i_x with Q equal to node->cmap.
 *
 * The constraints are those from the edges plus two or three equalities
 * to express the sums.
 *
 * If "use_coincidence" is set, then we treat coincidence edges as local edges.
 * Otherwise, we ignore them.
 */
static int setup_lp(isl_ctx *ctx, struct isl_sched_graph *graph,
	int use_coincidence)
{
	int i, j;
	int k;
	unsigned nparam;
	unsigned total;
	isl_space *dim;
	int parametric;
	int param_pos;
	int n_eq, n_ineq;
	int max_constant_term;

	max_constant_term = ctx->opt->schedule_max_constant_term;

	parametric = ctx->opt->schedule_parametric;
	nparam = isl_space_dim(graph->node[0].dim, isl_dim_param);
	param_pos = 4;
	total = param_pos + 2 * nparam;
	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[graph->sorted[i]];
		if (node_update_cmap(node) < 0)
			return -1;
		node->start = total;
		total += 1 + 2 * (node->nparam + node->nvar);
	}

	if (count_constraints(graph, &n_eq, &n_ineq, use_coincidence) < 0)
		return -1;
	if (count_bound_coefficient_constraints(ctx, graph, &n_eq, &n_ineq) < 0)
		return -1;

	dim = isl_space_set_alloc(ctx, 0, total);
	isl_basic_set_free(graph->lp);
	n_eq += 2 + parametric;
	if (max_constant_term != -1)
		n_ineq += graph->n;

	graph->lp = isl_basic_set_alloc_space(dim, 0, n_eq, n_ineq);

	k = isl_basic_set_alloc_equality(graph->lp);
	if (k < 0)
		return -1;
	isl_seq_clr(graph->lp->eq[k], 1 +  total);
	isl_int_set_si(graph->lp->eq[k][1], -1);
	for (i = 0; i < 2 * nparam; ++i)
		isl_int_set_si(graph->lp->eq[k][1 + param_pos + i], 1);

	if (parametric) {
		k = isl_basic_set_alloc_equality(graph->lp);
		if (k < 0)
			return -1;
		isl_seq_clr(graph->lp->eq[k], 1 +  total);
		isl_int_set_si(graph->lp->eq[k][3], -1);
		for (i = 0; i < graph->n; ++i) {
			int pos = 1 + graph->node[i].start + 1;

			for (j = 0; j < 2 * graph->node[i].nparam; ++j)
				isl_int_set_si(graph->lp->eq[k][pos + j], 1);
		}
	}

	k = isl_basic_set_alloc_equality(graph->lp);
	if (k < 0)
		return -1;
	isl_seq_clr(graph->lp->eq[k], 1 +  total);
	isl_int_set_si(graph->lp->eq[k][4], -1);
	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int pos = 1 + node->start + 1 + 2 * node->nparam;

		for (j = 0; j < 2 * node->nvar; ++j)
			isl_int_set_si(graph->lp->eq[k][pos + j], 1);
	}

	if (max_constant_term != -1)
		for (i = 0; i < graph->n; ++i) {
			struct isl_sched_node *node = &graph->node[i];
			k = isl_basic_set_alloc_inequality(graph->lp);
			if (k < 0)
				return -1;
			isl_seq_clr(graph->lp->ineq[k], 1 +  total);
			isl_int_set_si(graph->lp->ineq[k][1 + node->start], -1);
			isl_int_set_si(graph->lp->ineq[k][0], max_constant_term);
		}

	if (add_bound_coefficient_constraints(ctx, graph) < 0)
		return -1;
	if (add_all_validity_constraints(graph, use_coincidence) < 0)
		return -1;
	if (add_all_proximity_constraints(graph, use_coincidence) < 0)
		return -1;

	return 0;
}

/* Analyze the conflicting constraint found by
 * isl_tab_basic_set_non_trivial_lexmin.  If it corresponds to the validity
 * constraint of one of the edges between distinct nodes, living, moreover
 * in distinct SCCs, then record the source and sink SCC as this may
 * be a good place to cut between SCCs.
 */
static int check_conflict(int con, void *user)
{
	int i;
	struct isl_sched_graph *graph = user;

	if (graph->src_scc >= 0)
		return 0;

	con -= graph->lp->n_eq;

	if (con >= graph->lp->n_ineq)
		return 0;

	for (i = 0; i < graph->n_edge; ++i) {
		if (!graph->edge[i].validity)
			continue;
		if (graph->edge[i].src == graph->edge[i].dst)
			continue;
		if (graph->edge[i].src->scc == graph->edge[i].dst->scc)
			continue;
		if (graph->edge[i].start > con)
			continue;
		if (graph->edge[i].end <= con)
			continue;
		graph->src_scc = graph->edge[i].src->scc;
		graph->dst_scc = graph->edge[i].dst->scc;
	}

	return 0;
}

/* Check whether the next schedule row of the given node needs to be
 * non-trivial.  Lower-dimensional domains may have some trivial rows,
 * but as soon as the number of remaining required non-trivial rows
 * is as large as the number or remaining rows to be computed,
 * all remaining rows need to be non-trivial.
 */
static int needs_row(struct isl_sched_graph *graph, struct isl_sched_node *node)
{
	return node->nvar - node->rank >= graph->maxvar - graph->n_row;
}

/* Solve the ILP problem constructed in setup_lp.
 * For each node such that all the remaining rows of its schedule
 * need to be non-trivial, we construct a non-triviality region.
 * This region imposes that the next row is independent of previous rows.
 * In particular the coefficients c_i_x are represented by t_i_x
 * variables with c_i_x = Q t_i_x and Q a unimodular matrix such that
 * its first columns span the rows of the previously computed part
 * of the schedule.  The non-triviality region enforces that at least
 * one of the remaining components of t_i_x is non-zero, i.e.,
 * that the new schedule row depends on at least one of the remaining
 * columns of Q.
 */
static __isl_give isl_vec *solve_lp(struct isl_sched_graph *graph)
{
	int i;
	isl_vec *sol;
	isl_basic_set *lp;

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int skip = node->rank;
		graph->region[i].pos = node->start + 1 + 2*(node->nparam+skip);
		if (needs_row(graph, node))
			graph->region[i].len = 2 * (node->nvar - skip);
		else
			graph->region[i].len = 0;
	}
	lp = isl_basic_set_copy(graph->lp);
	sol = isl_tab_basic_set_non_trivial_lexmin(lp, 2, graph->n,
				       graph->region, &check_conflict, graph);
	return sol;
}

/* Update the schedules of all nodes based on the given solution
 * of the LP problem.
 * The new row is added to the current band.
 * All possibly negative coefficients are encoded as a difference
 * of two non-negative variables, so we need to perform the subtraction
 * here.  Moreover, if use_cmap is set, then the solution does
 * not refer to the actual coefficients c_i_x, but instead to variables
 * t_i_x such that c_i_x = Q t_i_x and Q is equal to node->cmap.
 * In this case, we then also need to perform this multiplication
 * to obtain the values of c_i_x.
 *
 * If coincident is set, then the caller guarantees that the new
 * row satisfies the coincidence constraints.
 */
static int update_schedule(struct isl_sched_graph *graph,
	__isl_take isl_vec *sol, int use_cmap, int coincident)
{
	int i, j;
	isl_vec *csol = NULL;

	if (!sol)
		goto error;
	if (sol->size == 0)
		isl_die(sol->ctx, isl_error_internal,
			"no solution found", goto error);
	if (graph->n_total_row >= graph->max_row)
		isl_die(sol->ctx, isl_error_internal,
			"too many schedule rows", goto error);

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int pos = node->start;
		int row = isl_mat_rows(node->sched);

		isl_vec_free(csol);
		csol = isl_vec_alloc(sol->ctx, node->nvar);
		if (!csol)
			goto error;

		isl_map_free(node->sched_map);
		node->sched_map = NULL;
		node->sched = isl_mat_add_rows(node->sched, 1);
		if (!node->sched)
			goto error;
		node->sched = isl_mat_set_element(node->sched, row, 0,
						  sol->el[1 + pos]);
		for (j = 0; j < node->nparam + node->nvar; ++j)
			isl_int_sub(sol->el[1 + pos + 1 + 2 * j + 1],
				    sol->el[1 + pos + 1 + 2 * j + 1],
				    sol->el[1 + pos + 1 + 2 * j]);
		for (j = 0; j < node->nparam; ++j)
			node->sched = isl_mat_set_element(node->sched,
					row, 1 + j, sol->el[1+pos+1+2*j+1]);
		for (j = 0; j < node->nvar; ++j)
			isl_int_set(csol->el[j],
				    sol->el[1+pos+1+2*(node->nparam+j)+1]);
		if (use_cmap)
			csol = isl_mat_vec_product(isl_mat_copy(node->cmap),
						   csol);
		if (!csol)
			goto error;
		for (j = 0; j < node->nvar; ++j)
			node->sched = isl_mat_set_element(node->sched,
					row, 1 + node->nparam + j, csol->el[j]);
		node->band[graph->n_total_row] = graph->n_band;
		node->coincident[graph->n_total_row] = coincident;
	}
	isl_vec_free(sol);
	isl_vec_free(csol);

	graph->n_row++;
	graph->n_total_row++;

	return 0;
error:
	isl_vec_free(sol);
	isl_vec_free(csol);
	return -1;
}

/* Convert row "row" of node->sched into an isl_aff living in "ls"
 * and return this isl_aff.
 */
static __isl_give isl_aff *extract_schedule_row(__isl_take isl_local_space *ls,
	struct isl_sched_node *node, int row)
{
	int j;
	isl_int v;
	isl_aff *aff;

	isl_int_init(v);

	aff = isl_aff_zero_on_domain(ls);
	isl_mat_get_element(node->sched, row, 0, &v);
	aff = isl_aff_set_constant(aff, v);
	for (j = 0; j < node->nparam; ++j) {
		isl_mat_get_element(node->sched, row, 1 + j, &v);
		aff = isl_aff_set_coefficient(aff, isl_dim_param, j, v);
	}
	for (j = 0; j < node->nvar; ++j) {
		isl_mat_get_element(node->sched, row, 1 + node->nparam + j, &v);
		aff = isl_aff_set_coefficient(aff, isl_dim_in, j, v);
	}

	isl_int_clear(v);

	return aff;
}

/* Convert node->sched into a multi_aff and return this multi_aff.
 */
static __isl_give isl_multi_aff *node_extract_schedule_multi_aff(
	struct isl_sched_node *node)
{
	int i;
	isl_space *space;
	isl_local_space *ls;
	isl_aff *aff;
	isl_multi_aff *ma;
	int nrow, ncol;

	nrow = isl_mat_rows(node->sched);
	ncol = isl_mat_cols(node->sched) - 1;
	space = isl_space_from_domain(isl_space_copy(node->dim));
	space = isl_space_add_dims(space, isl_dim_out, nrow);
	ma = isl_multi_aff_zero(space);
	ls = isl_local_space_from_space(isl_space_copy(node->dim));

	for (i = 0; i < nrow; ++i) {
		aff = extract_schedule_row(isl_local_space_copy(ls), node, i);
		ma = isl_multi_aff_set_aff(ma, i, aff);
	}

	isl_local_space_free(ls);

	return ma;
}

/* Convert node->sched into a map and return this map.
 *
 * The result is cached in node->sched_map, which needs to be released
 * whenever node->sched is updated.
 */
static __isl_give isl_map *node_extract_schedule(struct isl_sched_node *node)
{
	if (!node->sched_map) {
		isl_multi_aff *ma;

		ma = node_extract_schedule_multi_aff(node);
		node->sched_map = isl_map_from_multi_aff(ma);
	}

	return isl_map_copy(node->sched_map);
}

/* Construct a map that can be used to update a dependence relation
 * based on the current schedule.
 * That is, construct a map expressing that source and sink
 * are executed within the same iteration of the current schedule.
 * This map can then be intersected with the dependence relation.
 * This is not the most efficient way, but this shouldn't be a critical
 * operation.
 */
static __isl_give isl_map *specializer(struct isl_sched_node *src,
	struct isl_sched_node *dst)
{
	isl_map *src_sched, *dst_sched;

	src_sched = node_extract_schedule(src);
	dst_sched = node_extract_schedule(dst);
	return isl_map_apply_range(src_sched, isl_map_reverse(dst_sched));
}

/* Intersect the domains of the nested relations in domain and range
 * of "umap" with "map".
 */
static __isl_give isl_union_map *intersect_domains(
	__isl_take isl_union_map *umap, __isl_keep isl_map *map)
{
	isl_union_set *uset;

	umap = isl_union_map_zip(umap);
	uset = isl_union_set_from_set(isl_map_wrap(isl_map_copy(map)));
	umap = isl_union_map_intersect_domain(umap, uset);
	umap = isl_union_map_zip(umap);
	return umap;
}

/* Update the dependence relation of the given edge based
 * on the current schedule.
 * If the dependence is carried completely by the current schedule, then
 * it is removed from the edge_tables.  It is kept in the list of edges
 * as otherwise all edge_tables would have to be recomputed.
 */
static int update_edge(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge)
{
	isl_map *id;

	id = specializer(edge->src, edge->dst);
	edge->map = isl_map_intersect(edge->map, isl_map_copy(id));
	if (!edge->map)
		goto error;

	if (edge->tagged_condition) {
		edge->tagged_condition =
			intersect_domains(edge->tagged_condition, id);
		if (!edge->tagged_condition)
			goto error;
	}
	if (edge->tagged_validity) {
		edge->tagged_validity =
			intersect_domains(edge->tagged_validity, id);
		if (!edge->tagged_validity)
			goto error;
	}

	isl_map_free(id);
	if (isl_map_plain_is_empty(edge->map))
		graph_remove_edge(graph, edge);

	return 0;
error:
	isl_map_free(id);
	return -1;
}

/* Update the dependence relations of all edges based on the current schedule.
 */
static int update_edges(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i;

	for (i = graph->n_edge - 1; i >= 0; --i) {
		if (update_edge(graph, &graph->edge[i]) < 0)
			return -1;
	}

	return 0;
}

static void next_band(struct isl_sched_graph *graph)
{
	graph->band_start = graph->n_total_row;
	graph->n_band++;
}

/* Topologically sort statements mapped to the same schedule iteration
 * and add a row to the schedule corresponding to this order.
 */
static int sort_statements(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i, j;

	if (graph->n <= 1)
		return 0;

	if (update_edges(ctx, graph) < 0)
		return -1;

	if (graph->n_edge == 0)
		return 0;

	if (detect_sccs(ctx, graph) < 0)
		return -1;

	if (graph->n_total_row >= graph->max_row)
		isl_die(ctx, isl_error_internal,
			"too many schedule rows", return -1);

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int row = isl_mat_rows(node->sched);
		int cols = isl_mat_cols(node->sched);

		isl_map_free(node->sched_map);
		node->sched_map = NULL;
		node->sched = isl_mat_add_rows(node->sched, 1);
		if (!node->sched)
			return -1;
		node->sched = isl_mat_set_element_si(node->sched, row, 0,
						     node->scc);
		for (j = 1; j < cols; ++j)
			node->sched = isl_mat_set_element_si(node->sched,
							     row, j, 0);
		node->band[graph->n_total_row] = graph->n_band;
	}

	graph->n_total_row++;
	next_band(graph);

	return 0;
}

/* Construct an isl_schedule based on the computed schedule stored
 * in graph and with parameters specified by dim.
 */
static __isl_give isl_schedule *extract_schedule(struct isl_sched_graph *graph,
	__isl_take isl_space *dim)
{
	int i;
	isl_ctx *ctx;
	isl_schedule *sched = NULL;

	if (!dim)
		return NULL;

	ctx = isl_space_get_ctx(dim);
	sched = isl_calloc(ctx, struct isl_schedule,
			   sizeof(struct isl_schedule) +
			   (graph->n - 1) * sizeof(struct isl_schedule_node));
	if (!sched)
		goto error;

	sched->ref = 1;
	sched->n = graph->n;
	sched->n_band = graph->n_band;
	sched->n_total_row = graph->n_total_row;

	for (i = 0; i < sched->n; ++i) {
		int r, b;
		int *band_end, *band_id, *coincident;

		sched->node[i].sched =
			node_extract_schedule_multi_aff(&graph->node[i]);
		if (!sched->node[i].sched)
			goto error;

		sched->node[i].n_band = graph->n_band;
		if (graph->n_band == 0)
			continue;

		band_end = isl_alloc_array(ctx, int, graph->n_band);
		band_id = isl_alloc_array(ctx, int, graph->n_band);
		coincident = isl_alloc_array(ctx, int, graph->n_total_row);
		sched->node[i].band_end = band_end;
		sched->node[i].band_id = band_id;
		sched->node[i].coincident = coincident;
		if (!band_end || !band_id || !coincident)
			goto error;

		for (r = 0; r < graph->n_total_row; ++r)
			coincident[r] = graph->node[i].coincident[r];
		for (r = b = 0; r < graph->n_total_row; ++r) {
			if (graph->node[i].band[r] == b)
				continue;
			band_end[b++] = r;
			if (graph->node[i].band[r] == -1)
				break;
		}
		if (r == graph->n_total_row)
			band_end[b++] = r;
		sched->node[i].n_band = b;
		for (--b; b >= 0; --b)
			band_id[b] = graph->node[i].band_id[b];
	}

	sched->dim = dim;

	return sched;
error:
	isl_space_free(dim);
	isl_schedule_free(sched);
	return NULL;
}

/* Copy nodes that satisfy node_pred from the src dependence graph
 * to the dst dependence graph.
 */
static int copy_nodes(struct isl_sched_graph *dst, struct isl_sched_graph *src,
	int (*node_pred)(struct isl_sched_node *node, int data), int data)
{
	int i;

	dst->n = 0;
	for (i = 0; i < src->n; ++i) {
		int j;

		if (!node_pred(&src->node[i], data))
			continue;

		j = dst->n;
		dst->node[j].dim = isl_space_copy(src->node[i].dim);
		dst->node[j].nvar = src->node[i].nvar;
		dst->node[j].nparam = src->node[i].nparam;
		dst->node[j].sched = isl_mat_copy(src->node[i].sched);
		dst->node[j].sched_map = isl_map_copy(src->node[i].sched_map);
		dst->node[j].band = src->node[i].band;
		dst->node[j].band_id = src->node[i].band_id;
		dst->node[j].coincident = src->node[i].coincident;
		dst->n++;

		if (!dst->node[j].dim || !dst->node[j].sched)
			return -1;
	}

	return 0;
}

/* Copy non-empty edges that satisfy edge_pred from the src dependence graph
 * to the dst dependence graph.
 * If the source or destination node of the edge is not in the destination
 * graph, then it must be a backward proximity edge and it should simply
 * be ignored.
 */
static int copy_edges(isl_ctx *ctx, struct isl_sched_graph *dst,
	struct isl_sched_graph *src,
	int (*edge_pred)(struct isl_sched_edge *edge, int data), int data)
{
	int i;
	enum isl_edge_type t;

	dst->n_edge = 0;
	for (i = 0; i < src->n_edge; ++i) {
		struct isl_sched_edge *edge = &src->edge[i];
		isl_map *map;
		isl_union_map *tagged_condition;
		isl_union_map *tagged_validity;
		struct isl_sched_node *dst_src, *dst_dst;

		if (!edge_pred(edge, data))
			continue;

		if (isl_map_plain_is_empty(edge->map))
			continue;

		dst_src = graph_find_node(ctx, dst, edge->src->dim);
		dst_dst = graph_find_node(ctx, dst, edge->dst->dim);
		if (!dst_src || !dst_dst) {
			if (edge->validity || edge->conditional_validity)
				isl_die(ctx, isl_error_internal,
					"backward (conditional) validity edge",
					return -1);
			continue;
		}

		map = isl_map_copy(edge->map);
		tagged_condition = isl_union_map_copy(edge->tagged_condition);
		tagged_validity = isl_union_map_copy(edge->tagged_validity);

		dst->edge[dst->n_edge].src = dst_src;
		dst->edge[dst->n_edge].dst = dst_dst;
		dst->edge[dst->n_edge].map = map;
		dst->edge[dst->n_edge].tagged_condition = tagged_condition;
		dst->edge[dst->n_edge].tagged_validity = tagged_validity;
		dst->edge[dst->n_edge].validity = edge->validity;
		dst->edge[dst->n_edge].proximity = edge->proximity;
		dst->edge[dst->n_edge].coincidence = edge->coincidence;
		dst->edge[dst->n_edge].condition = edge->condition;
		dst->edge[dst->n_edge].conditional_validity =
						edge->conditional_validity;
		dst->n_edge++;

		if (edge->tagged_condition && !tagged_condition)
			return -1;
		if (edge->tagged_validity && !tagged_validity)
			return -1;

		for (t = isl_edge_first; t <= isl_edge_last; ++t) {
			if (edge !=
			    graph_find_edge(src, t, edge->src, edge->dst))
				continue;
			if (graph_edge_table_add(ctx, dst, t,
					    &dst->edge[dst->n_edge - 1]) < 0)
				return -1;
		}
	}

	return 0;
}

/* Given a "src" dependence graph that contains the nodes from "dst"
 * that satisfy node_pred, copy the schedule computed in "src"
 * for those nodes back to "dst".
 */
static int copy_schedule(struct isl_sched_graph *dst,
	struct isl_sched_graph *src,
	int (*node_pred)(struct isl_sched_node *node, int data), int data)
{
	int i;

	src->n = 0;
	for (i = 0; i < dst->n; ++i) {
		if (!node_pred(&dst->node[i], data))
			continue;
		isl_mat_free(dst->node[i].sched);
		isl_map_free(dst->node[i].sched_map);
		dst->node[i].sched = isl_mat_copy(src->node[src->n].sched);
		dst->node[i].sched_map =
			isl_map_copy(src->node[src->n].sched_map);
		src->n++;
	}

	dst->max_row = src->max_row;
	dst->n_total_row = src->n_total_row;
	dst->n_band = src->n_band;

	return 0;
}

/* Compute the maximal number of variables over all nodes.
 * This is the maximal number of linearly independent schedule
 * rows that we need to compute.
 * Just in case we end up in a part of the dependence graph
 * with only lower-dimensional domains, we make sure we will
 * compute the required amount of extra linearly independent rows.
 */
static int compute_maxvar(struct isl_sched_graph *graph)
{
	int i;

	graph->maxvar = 0;
	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int nvar;

		if (node_update_cmap(node) < 0)
			return -1;
		nvar = node->nvar + graph->n_row - node->rank;
		if (nvar > graph->maxvar)
			graph->maxvar = nvar;
	}

	return 0;
}

static int compute_schedule(isl_ctx *ctx, struct isl_sched_graph *graph);
static int compute_schedule_wcc(isl_ctx *ctx, struct isl_sched_graph *graph);

/* Compute a schedule for a subgraph of "graph".  In particular, for
 * the graph composed of nodes that satisfy node_pred and edges that
 * that satisfy edge_pred.  The caller should precompute the number
 * of nodes and edges that satisfy these predicates and pass them along
 * as "n" and "n_edge".
 * If the subgraph is known to consist of a single component, then wcc should
 * be set and then we call compute_schedule_wcc on the constructed subgraph.
 * Otherwise, we call compute_schedule, which will check whether the subgraph
 * is connected.
 */
static int compute_sub_schedule(isl_ctx *ctx,
	struct isl_sched_graph *graph, int n, int n_edge,
	int (*node_pred)(struct isl_sched_node *node, int data),
	int (*edge_pred)(struct isl_sched_edge *edge, int data),
	int data, int wcc)
{
	struct isl_sched_graph split = { 0 };
	int t;

	if (graph_alloc(ctx, &split, n, n_edge) < 0)
		goto error;
	if (copy_nodes(&split, graph, node_pred, data) < 0)
		goto error;
	if (graph_init_table(ctx, &split) < 0)
		goto error;
	for (t = 0; t <= isl_edge_last; ++t)
		split.max_edge[t] = graph->max_edge[t];
	if (graph_init_edge_tables(ctx, &split) < 0)
		goto error;
	if (copy_edges(ctx, &split, graph, edge_pred, data) < 0)
		goto error;
	split.n_row = graph->n_row;
	split.max_row = graph->max_row;
	split.n_total_row = graph->n_total_row;
	split.n_band = graph->n_band;
	split.band_start = graph->band_start;

	if (wcc && compute_schedule_wcc(ctx, &split) < 0)
		goto error;
	if (!wcc && compute_schedule(ctx, &split) < 0)
		goto error;

	copy_schedule(graph, &split, node_pred, data);

	graph_free(ctx, &split);
	return 0;
error:
	graph_free(ctx, &split);
	return -1;
}

static int node_scc_exactly(struct isl_sched_node *node, int scc)
{
	return node->scc == scc;
}

static int node_scc_at_most(struct isl_sched_node *node, int scc)
{
	return node->scc <= scc;
}

static int node_scc_at_least(struct isl_sched_node *node, int scc)
{
	return node->scc >= scc;
}

static int edge_scc_exactly(struct isl_sched_edge *edge, int scc)
{
	return edge->src->scc == scc && edge->dst->scc == scc;
}

static int edge_dst_scc_at_most(struct isl_sched_edge *edge, int scc)
{
	return edge->dst->scc <= scc;
}

static int edge_src_scc_at_least(struct isl_sched_edge *edge, int scc)
{
	return edge->src->scc >= scc;
}

/* Pad the schedules of all nodes with zero rows such that in the end
 * they all have graph->n_total_row rows.
 * The extra rows don't belong to any band, so they get assigned band number -1.
 */
static int pad_schedule(struct isl_sched_graph *graph)
{
	int i, j;

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int row = isl_mat_rows(node->sched);
		if (graph->n_total_row > row) {
			isl_map_free(node->sched_map);
			node->sched_map = NULL;
		}
		node->sched = isl_mat_add_zero_rows(node->sched,
						    graph->n_total_row - row);
		if (!node->sched)
			return -1;
		for (j = row; j < graph->n_total_row; ++j)
			node->band[j] = -1;
	}

	return 0;
}

/* Reset the current band by dropping all its schedule rows.
 */
static int reset_band(struct isl_sched_graph *graph)
{
	int i;
	int drop;

	drop = graph->n_total_row - graph->band_start;
	graph->n_total_row -= drop;
	graph->n_row -= drop;

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];

		isl_map_free(node->sched_map);
		node->sched_map = NULL;

		node->sched = isl_mat_drop_rows(node->sched,
						graph->band_start, drop);

		if (!node->sched)
			return -1;
	}

	return 0;
}

/* Split the current graph into two parts and compute a schedule for each
 * part individually.  In particular, one part consists of all SCCs up
 * to and including graph->src_scc, while the other part contains the other
 * SCCS.
 *
 * The split is enforced in the schedule by constant rows with two different
 * values (0 and 1).  These constant rows replace the previously computed rows
 * in the current band.
 * It would be possible to reuse them as the first rows in the next
 * band, but recomputing them may result in better rows as we are looking
 * at a smaller part of the dependence graph.
 *
 * Since we do not enforce coincidence, we conservatively mark the
 * splitting row as not coincident.
 *
 * The band_id of the second group is set to n, where n is the number
 * of nodes in the first group.  This ensures that the band_ids over
 * the two groups remain disjoint, even if either or both of the two
 * groups contain independent components.
 */
static int compute_split_schedule(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i, j, n, e1, e2;
	int n_total_row, orig_total_row;
	int n_band, orig_band;

	if (graph->n_total_row >= graph->max_row)
		isl_die(ctx, isl_error_internal,
			"too many schedule rows", return -1);

	if (reset_band(graph) < 0)
		return -1;

	n = 0;
	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int row = isl_mat_rows(node->sched);
		int cols = isl_mat_cols(node->sched);
		int before = node->scc <= graph->src_scc;

		if (before)
			n++;

		isl_map_free(node->sched_map);
		node->sched_map = NULL;
		node->sched = isl_mat_add_rows(node->sched, 1);
		if (!node->sched)
			return -1;
		node->sched = isl_mat_set_element_si(node->sched, row, 0,
						     !before);
		for (j = 1; j < cols; ++j)
			node->sched = isl_mat_set_element_si(node->sched,
							     row, j, 0);
		node->band[graph->n_total_row] = graph->n_band;
		node->coincident[graph->n_total_row] = 0;
	}

	e1 = e2 = 0;
	for (i = 0; i < graph->n_edge; ++i) {
		if (graph->edge[i].dst->scc <= graph->src_scc)
			e1++;
		if (graph->edge[i].src->scc > graph->src_scc)
			e2++;
	}

	graph->n_total_row++;
	next_band(graph);

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		if (node->scc > graph->src_scc)
			node->band_id[graph->n_band] = n;
	}

	orig_total_row = graph->n_total_row;
	orig_band = graph->n_band;
	if (compute_sub_schedule(ctx, graph, n, e1,
				&node_scc_at_most, &edge_dst_scc_at_most,
				graph->src_scc, 0) < 0)
		return -1;
	n_total_row = graph->n_total_row;
	graph->n_total_row = orig_total_row;
	n_band = graph->n_band;
	graph->n_band = orig_band;
	if (compute_sub_schedule(ctx, graph, graph->n - n, e2,
				&node_scc_at_least, &edge_src_scc_at_least,
				graph->src_scc + 1, 0) < 0)
		return -1;
	if (n_total_row > graph->n_total_row)
		graph->n_total_row = n_total_row;
	if (n_band > graph->n_band)
		graph->n_band = n_band;

	return pad_schedule(graph);
}

/* Compute the next band of the schedule after updating the dependence
 * relations based on the the current schedule.
 */
static int compute_next_band(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	if (update_edges(ctx, graph) < 0)
		return -1;
	next_band(graph);

	return compute_schedule(ctx, graph);
}

/* Add constraints to graph->lp that force the dependence "map" (which
 * is part of the dependence relation of "edge")
 * to be respected and attempt to carry it, where the edge is one from
 * a node j to itself.  "pos" is the sequence number of the given map.
 * That is, add constraints that enforce
 *
 *	(c_j_0 + c_j_n n + c_j_x y) - (c_j_0 + c_j_n n + c_j_x x)
 *	= c_j_x (y - x) >= e_i
 *
 * for each (x,y) in R.
 * We obtain general constraints on coefficients (c_0, c_n, c_x)
 * of valid constraints for (y - x) and then plug in (-e_i, 0, c_j_x),
 * with each coefficient in c_j_x represented as a pair of non-negative
 * coefficients.
 */
static int add_intra_constraints(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge, __isl_take isl_map *map, int pos)
{
	unsigned total;
	isl_ctx *ctx = isl_map_get_ctx(map);
	isl_space *dim;
	isl_dim_map *dim_map;
	isl_basic_set *coef;
	struct isl_sched_node *node = edge->src;

	coef = intra_coefficients(graph, map);
	if (!coef)
		return -1;

	dim = isl_space_domain(isl_space_unwrap(isl_basic_set_get_space(coef)));

	total = isl_basic_set_total_dim(graph->lp);
	dim_map = isl_dim_map_alloc(ctx, total);
	isl_dim_map_range(dim_map, 3 + pos, 0, 0, 0, 1, -1);
	isl_dim_map_range(dim_map, node->start + 2 * node->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  node->nvar, -1);
	isl_dim_map_range(dim_map, node->start + 2 * node->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  node->nvar, 1);
	graph->lp = isl_basic_set_extend_constraints(graph->lp,
			coef->n_eq, coef->n_ineq);
	graph->lp = isl_basic_set_add_constraints_dim_map(graph->lp,
							   coef, dim_map);
	isl_space_free(dim);

	return 0;
}

/* Add constraints to graph->lp that force the dependence "map" (which
 * is part of the dependence relation of "edge")
 * to be respected and attempt to carry it, where the edge is one from
 * node j to node k.  "pos" is the sequence number of the given map.
 * That is, add constraints that enforce
 *
 *	(c_k_0 + c_k_n n + c_k_x y) - (c_j_0 + c_j_n n + c_j_x x) >= e_i
 *
 * for each (x,y) in R.
 * We obtain general constraints on coefficients (c_0, c_n, c_x)
 * of valid constraints for R and then plug in
 * (-e_i + c_k_0 - c_j_0, c_k_n - c_j_n, c_k_x - c_j_x)
 * with each coefficient (except e_i, c_k_0 and c_j_0)
 * represented as a pair of non-negative coefficients.
 */
static int add_inter_constraints(struct isl_sched_graph *graph,
	struct isl_sched_edge *edge, __isl_take isl_map *map, int pos)
{
	unsigned total;
	isl_ctx *ctx = isl_map_get_ctx(map);
	isl_space *dim;
	isl_dim_map *dim_map;
	isl_basic_set *coef;
	struct isl_sched_node *src = edge->src;
	struct isl_sched_node *dst = edge->dst;

	coef = inter_coefficients(graph, map);
	if (!coef)
		return -1;

	dim = isl_space_domain(isl_space_unwrap(isl_basic_set_get_space(coef)));

	total = isl_basic_set_total_dim(graph->lp);
	dim_map = isl_dim_map_alloc(ctx, total);

	isl_dim_map_range(dim_map, 3 + pos, 0, 0, 0, 1, -1);

	isl_dim_map_range(dim_map, dst->start, 0, 0, 0, 1, 1);
	isl_dim_map_range(dim_map, dst->start + 1, 2, 1, 1, dst->nparam, -1);
	isl_dim_map_range(dim_map, dst->start + 2, 2, 1, 1, dst->nparam, 1);
	isl_dim_map_range(dim_map, dst->start + 2 * dst->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set) + src->nvar, 1,
			  dst->nvar, -1);
	isl_dim_map_range(dim_map, dst->start + 2 * dst->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set) + src->nvar, 1,
			  dst->nvar, 1);

	isl_dim_map_range(dim_map, src->start, 0, 0, 0, 1, -1);
	isl_dim_map_range(dim_map, src->start + 1, 2, 1, 1, src->nparam, 1);
	isl_dim_map_range(dim_map, src->start + 2, 2, 1, 1, src->nparam, -1);
	isl_dim_map_range(dim_map, src->start + 2 * src->nparam + 1, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  src->nvar, 1);
	isl_dim_map_range(dim_map, src->start + 2 * src->nparam + 2, 2,
			  isl_space_dim(dim, isl_dim_set), 1,
			  src->nvar, -1);

	graph->lp = isl_basic_set_extend_constraints(graph->lp,
			coef->n_eq, coef->n_ineq);
	graph->lp = isl_basic_set_add_constraints_dim_map(graph->lp,
							   coef, dim_map);
	isl_space_free(dim);

	return 0;
}

/* Add constraints to graph->lp that force all (conditional) validity
 * dependences to be respected and attempt to carry them.
 */
static int add_all_constraints(struct isl_sched_graph *graph)
{
	int i, j;
	int pos;

	pos = 0;
	for (i = 0; i < graph->n_edge; ++i) {
		struct isl_sched_edge *edge= &graph->edge[i];

		if (!edge->validity && !edge->conditional_validity)
			continue;

		for (j = 0; j < edge->map->n; ++j) {
			isl_basic_map *bmap;
			isl_map *map;

			bmap = isl_basic_map_copy(edge->map->p[j]);
			map = isl_map_from_basic_map(bmap);

			if (edge->src == edge->dst &&
			    add_intra_constraints(graph, edge, map, pos) < 0)
				return -1;
			if (edge->src != edge->dst &&
			    add_inter_constraints(graph, edge, map, pos) < 0)
				return -1;
			++pos;
		}
	}

	return 0;
}

/* Count the number of equality and inequality constraints
 * that will be added to the carry_lp problem.
 * We count each edge exactly once.
 */
static int count_all_constraints(struct isl_sched_graph *graph,
	int *n_eq, int *n_ineq)
{
	int i, j;

	*n_eq = *n_ineq = 0;
	for (i = 0; i < graph->n_edge; ++i) {
		struct isl_sched_edge *edge= &graph->edge[i];
		for (j = 0; j < edge->map->n; ++j) {
			isl_basic_map *bmap;
			isl_map *map;

			bmap = isl_basic_map_copy(edge->map->p[j]);
			map = isl_map_from_basic_map(bmap);

			if (count_map_constraints(graph, edge, map,
						  n_eq, n_ineq, 1, 0) < 0)
				    return -1;
		}
	}

	return 0;
}

/* Construct an LP problem for finding schedule coefficients
 * such that the schedule carries as many dependences as possible.
 * In particular, for each dependence i, we bound the dependence distance
 * from below by e_i, with 0 <= e_i <= 1 and then maximize the sum
 * of all e_i's.  Dependence with e_i = 0 in the solution are simply
 * respected, while those with e_i > 0 (in practice e_i = 1) are carried.
 * Note that if the dependence relation is a union of basic maps,
 * then we have to consider each basic map individually as it may only
 * be possible to carry the dependences expressed by some of those
 * basic maps and not all off them.
 * Below, we consider each of those basic maps as a separate "edge".
 *
 * All variables of the LP are non-negative.  The actual coefficients
 * may be negative, so each coefficient is represented as the difference
 * of two non-negative variables.  The negative part always appears
 * immediately before the positive part.
 * Other than that, the variables have the following order
 *
 *	- sum of (1 - e_i) over all edges
 *	- sum of positive and negative parts of all c_n coefficients
 *		(unconstrained when computing non-parametric schedules)
 *	- sum of positive and negative parts of all c_x coefficients
 *	- for each edge
 *		- e_i
 *	- for each node
 *		- c_i_0
 *		- positive and negative parts of c_i_n (if parametric)
 *		- positive and negative parts of c_i_x
 *
 * The constraints are those from the (validity) edges plus three equalities
 * to express the sums and n_edge inequalities to express e_i <= 1.
 */
static int setup_carry_lp(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i, j;
	int k;
	isl_space *dim;
	unsigned total;
	int n_eq, n_ineq;
	int n_edge;

	n_edge = 0;
	for (i = 0; i < graph->n_edge; ++i)
		n_edge += graph->edge[i].map->n;

	total = 3 + n_edge;
	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[graph->sorted[i]];
		node->start = total;
		total += 1 + 2 * (node->nparam + node->nvar);
	}

	if (count_all_constraints(graph, &n_eq, &n_ineq) < 0)
		return -1;
	if (count_bound_coefficient_constraints(ctx, graph, &n_eq, &n_ineq) < 0)
		return -1;

	dim = isl_space_set_alloc(ctx, 0, total);
	isl_basic_set_free(graph->lp);
	n_eq += 3;
	n_ineq += n_edge;
	graph->lp = isl_basic_set_alloc_space(dim, 0, n_eq, n_ineq);
	graph->lp = isl_basic_set_set_rational(graph->lp);

	k = isl_basic_set_alloc_equality(graph->lp);
	if (k < 0)
		return -1;
	isl_seq_clr(graph->lp->eq[k], 1 +  total);
	isl_int_set_si(graph->lp->eq[k][0], -n_edge);
	isl_int_set_si(graph->lp->eq[k][1], 1);
	for (i = 0; i < n_edge; ++i)
		isl_int_set_si(graph->lp->eq[k][4 + i], 1);

	k = isl_basic_set_alloc_equality(graph->lp);
	if (k < 0)
		return -1;
	isl_seq_clr(graph->lp->eq[k], 1 +  total);
	isl_int_set_si(graph->lp->eq[k][2], -1);
	for (i = 0; i < graph->n; ++i) {
		int pos = 1 + graph->node[i].start + 1;

		for (j = 0; j < 2 * graph->node[i].nparam; ++j)
			isl_int_set_si(graph->lp->eq[k][pos + j], 1);
	}

	k = isl_basic_set_alloc_equality(graph->lp);
	if (k < 0)
		return -1;
	isl_seq_clr(graph->lp->eq[k], 1 +  total);
	isl_int_set_si(graph->lp->eq[k][3], -1);
	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int pos = 1 + node->start + 1 + 2 * node->nparam;

		for (j = 0; j < 2 * node->nvar; ++j)
			isl_int_set_si(graph->lp->eq[k][pos + j], 1);
	}

	for (i = 0; i < n_edge; ++i) {
		k = isl_basic_set_alloc_inequality(graph->lp);
		if (k < 0)
			return -1;
		isl_seq_clr(graph->lp->ineq[k], 1 +  total);
		isl_int_set_si(graph->lp->ineq[k][4 + i], -1);
		isl_int_set_si(graph->lp->ineq[k][0], 1);
	}

	if (add_bound_coefficient_constraints(ctx, graph) < 0)
		return -1;
	if (add_all_constraints(graph) < 0)
		return -1;

	return 0;
}

/* If the schedule_split_scaled option is set and if the linear
 * parts of the scheduling rows for all nodes in the graphs have
 * non-trivial common divisor, then split off the constant term
 * from the linear part.
 * The constant term is then placed in a separate band and
 * the linear part is reduced.
 */
static int split_scaled(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i;
	int row;
	isl_int gcd, gcd_i;

	if (!ctx->opt->schedule_split_scaled)
		return 0;
	if (graph->n <= 1)
		return 0;

	if (graph->n_total_row >= graph->max_row)
		isl_die(ctx, isl_error_internal,
			"too many schedule rows", return -1);

	isl_int_init(gcd);
	isl_int_init(gcd_i);

	isl_int_set_si(gcd, 0);

	row = isl_mat_rows(graph->node[0].sched) - 1;

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int cols = isl_mat_cols(node->sched);

		isl_seq_gcd(node->sched->row[row] + 1, cols - 1, &gcd_i);
		isl_int_gcd(gcd, gcd, gcd_i);
	}

	isl_int_clear(gcd_i);

	if (isl_int_cmp_si(gcd, 1) <= 0) {
		isl_int_clear(gcd);
		return 0;
	}

	next_band(graph);

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];

		isl_map_free(node->sched_map);
		node->sched_map = NULL;
		node->sched = isl_mat_add_zero_rows(node->sched, 1);
		if (!node->sched)
			goto error;
		isl_int_fdiv_r(node->sched->row[row + 1][0],
			       node->sched->row[row][0], gcd);
		isl_int_fdiv_q(node->sched->row[row][0],
			       node->sched->row[row][0], gcd);
		isl_int_mul(node->sched->row[row][0],
			    node->sched->row[row][0], gcd);
		node->sched = isl_mat_scale_down_row(node->sched, row, gcd);
		if (!node->sched)
			goto error;
		node->band[graph->n_total_row] = graph->n_band;
	}

	graph->n_total_row++;

	isl_int_clear(gcd);
	return 0;
error:
	isl_int_clear(gcd);
	return -1;
}

static int compute_component_schedule(isl_ctx *ctx,
	struct isl_sched_graph *graph);

/* Is the schedule row "sol" trivial on node "node"?
 * That is, is the solution zero on the dimensions orthogonal to
 * the previously found solutions?
 * Return 1 if the solution is trivial, 0 if it is not and -1 on error.
 *
 * Each coefficient is represented as the difference between
 * two non-negative values in "sol".  "sol" has been computed
 * in terms of the original iterators (i.e., without use of cmap).
 * We construct the schedule row s and write it as a linear
 * combination of (linear combinations of) previously computed schedule rows.
 * s = Q c or c = U s.
 * If the final entries of c are all zero, then the solution is trivial.
 */
static int is_trivial(struct isl_sched_node *node, __isl_keep isl_vec *sol)
{
	int i;
	int pos;
	int trivial;
	isl_ctx *ctx;
	isl_vec *node_sol;

	if (!sol)
		return -1;
	if (node->nvar == node->rank)
		return 0;

	ctx = isl_vec_get_ctx(sol);
	node_sol = isl_vec_alloc(ctx, node->nvar);
	if (!node_sol)
		return -1;

	pos = 1 + node->start + 1 + 2 * node->nparam;

	for (i = 0; i < node->nvar; ++i)
		isl_int_sub(node_sol->el[i],
			    sol->el[pos + 2 * i + 1], sol->el[pos + 2 * i]);

	node_sol = isl_mat_vec_product(isl_mat_copy(node->cinv), node_sol);

	if (!node_sol)
		return -1;

	trivial = isl_seq_first_non_zero(node_sol->el + node->rank,
					node->nvar - node->rank) == -1;

	isl_vec_free(node_sol);

	return trivial;
}

/* Is the schedule row "sol" trivial on any node where it should
 * not be trivial?
 * "sol" has been computed in terms of the original iterators
 * (i.e., without use of cmap).
 * Return 1 if any solution is trivial, 0 if they are not and -1 on error.
 */
static int is_any_trivial(struct isl_sched_graph *graph,
	__isl_keep isl_vec *sol)
{
	int i;

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int trivial;

		if (!needs_row(graph, node))
			continue;
		trivial = is_trivial(node, sol);
		if (trivial < 0 || trivial)
			return trivial;
	}

	return 0;
}

/* Construct a schedule row for each node such that as many dependences
 * as possible are carried and then continue with the next band.
 *
 * If the computed schedule row turns out to be trivial on one or
 * more nodes where it should not be trivial, then we throw it away
 * and try again on each component separately.
 */
static int carry_dependences(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i;
	int n_edge;
	int trivial;
	isl_vec *sol;
	isl_basic_set *lp;

	n_edge = 0;
	for (i = 0; i < graph->n_edge; ++i)
		n_edge += graph->edge[i].map->n;

	if (setup_carry_lp(ctx, graph) < 0)
		return -1;

	lp = isl_basic_set_copy(graph->lp);
	sol = isl_tab_basic_set_non_neg_lexmin(lp);
	if (!sol)
		return -1;

	if (sol->size == 0) {
		isl_vec_free(sol);
		isl_die(ctx, isl_error_internal,
			"error in schedule construction", return -1);
	}

	isl_int_divexact(sol->el[1], sol->el[1], sol->el[0]);
	if (isl_int_cmp_si(sol->el[1], n_edge) >= 0) {
		isl_vec_free(sol);
		isl_die(ctx, isl_error_unknown,
			"unable to carry dependences", return -1);
	}

	trivial = is_any_trivial(graph, sol);
	if (trivial < 0) {
		sol = isl_vec_free(sol);
	} else if (trivial) {
		isl_vec_free(sol);
		if (graph->scc > 1)
			return compute_component_schedule(ctx, graph);
		isl_die(ctx, isl_error_unknown,
			"unable to construct non-trivial solution", return -1);
	}

	if (update_schedule(graph, sol, 0, 0) < 0)
		return -1;

	if (split_scaled(ctx, graph) < 0)
		return -1;

	return compute_next_band(ctx, graph);
}

/* Are there any (non-empty) (conditional) validity edges in the graph?
 */
static int has_validity_edges(struct isl_sched_graph *graph)
{
	int i;

	for (i = 0; i < graph->n_edge; ++i) {
		int empty;

		empty = isl_map_plain_is_empty(graph->edge[i].map);
		if (empty < 0)
			return -1;
		if (empty)
			continue;
		if (graph->edge[i].validity ||
		    graph->edge[i].conditional_validity)
			return 1;
	}

	return 0;
}

/* Should we apply a Feautrier step?
 * That is, did the user request the Feautrier algorithm and are
 * there any validity dependences (left)?
 */
static int need_feautrier_step(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	if (ctx->opt->schedule_algorithm != ISL_SCHEDULE_ALGORITHM_FEAUTRIER)
		return 0;

	return has_validity_edges(graph);
}

/* Compute a schedule for a connected dependence graph using Feautrier's
 * multi-dimensional scheduling algorithm.
 * The original algorithm is described in [1].
 * The main idea is to minimize the number of scheduling dimensions, by
 * trying to satisfy as many dependences as possible per scheduling dimension.
 *
 * [1] P. Feautrier, Some Efficient Solutions to the Affine Scheduling
 *     Problem, Part II: Multi-Dimensional Time.
 *     In Intl. Journal of Parallel Programming, 1992.
 */
static int compute_schedule_wcc_feautrier(isl_ctx *ctx,
	struct isl_sched_graph *graph)
{
	return carry_dependences(ctx, graph);
}

/* Turn off the "local" bit on all (condition) edges.
 */
static void clear_local_edges(struct isl_sched_graph *graph)
{
	int i;

	for (i = 0; i < graph->n_edge; ++i)
		if (graph->edge[i].condition)
			graph->edge[i].local = 0;
}

/* Does "graph" have both condition and conditional validity edges?
 */
static int need_condition_check(struct isl_sched_graph *graph)
{
	int i;
	int any_condition = 0;
	int any_conditional_validity = 0;

	for (i = 0; i < graph->n_edge; ++i) {
		if (graph->edge[i].condition)
			any_condition = 1;
		if (graph->edge[i].conditional_validity)
			any_conditional_validity = 1;
	}

	return any_condition && any_conditional_validity;
}

/* Does "graph" contain any coincidence edge?
 */
static int has_any_coincidence(struct isl_sched_graph *graph)
{
	int i;

	for (i = 0; i < graph->n_edge; ++i)
		if (graph->edge[i].coincidence)
			return 1;

	return 0;
}

/* Extract the final schedule row as a map with the iteration domain
 * of "node" as domain.
 */
static __isl_give isl_map *final_row(struct isl_sched_node *node)
{
	isl_local_space *ls;
	isl_aff *aff;
	int row;

	row = isl_mat_rows(node->sched) - 1;
	ls = isl_local_space_from_space(isl_space_copy(node->dim));
	aff = extract_schedule_row(ls, node, row);
	return isl_map_from_aff(aff);
}

/* Is the conditional validity dependence in the edge with index "edge_index"
 * violated by the latest (i.e., final) row of the schedule?
 * That is, is i scheduled after j
 * for any conditional validity dependence i -> j?
 */
static int is_violated(struct isl_sched_graph *graph, int edge_index)
{
	isl_map *src_sched, *dst_sched, *map;
	struct isl_sched_edge *edge = &graph->edge[edge_index];
	int empty;

	src_sched = final_row(edge->src);
	dst_sched = final_row(edge->dst);
	map = isl_map_copy(edge->map);
	map = isl_map_apply_domain(map, src_sched);
	map = isl_map_apply_range(map, dst_sched);
	map = isl_map_order_gt(map, isl_dim_in, 0, isl_dim_out, 0);
	empty = isl_map_is_empty(map);
	isl_map_free(map);

	if (empty < 0)
		return -1;

	return !empty;
}

/* Does the domain of "umap" intersect "uset"?
 */
static int domain_intersects(__isl_keep isl_union_map *umap,
	__isl_keep isl_union_set *uset)
{
	int empty;

	umap = isl_union_map_copy(umap);
	umap = isl_union_map_intersect_domain(umap, isl_union_set_copy(uset));
	empty = isl_union_map_is_empty(umap);
	isl_union_map_free(umap);

	return empty < 0 ? -1 : !empty;
}

/* Does the range of "umap" intersect "uset"?
 */
static int range_intersects(__isl_keep isl_union_map *umap,
	__isl_keep isl_union_set *uset)
{
	int empty;

	umap = isl_union_map_copy(umap);
	umap = isl_union_map_intersect_range(umap, isl_union_set_copy(uset));
	empty = isl_union_map_is_empty(umap);
	isl_union_map_free(umap);

	return empty < 0 ? -1 : !empty;
}

/* Are the condition dependences of "edge" local with respect to
 * the current schedule?
 *
 * That is, are domain and range of the condition dependences mapped
 * to the same point?
 *
 * In other words, is the condition false?
 */
static int is_condition_false(struct isl_sched_edge *edge)
{
	isl_union_map *umap;
	isl_map *map, *sched, *test;
	int local;

	umap = isl_union_map_copy(edge->tagged_condition);
	umap = isl_union_map_zip(umap);
	umap = isl_union_set_unwrap(isl_union_map_domain(umap));
	map = isl_map_from_union_map(umap);

	sched = node_extract_schedule(edge->src);
	map = isl_map_apply_domain(map, sched);
	sched = node_extract_schedule(edge->dst);
	map = isl_map_apply_range(map, sched);

	test = isl_map_identity(isl_map_get_space(map));
	local = isl_map_is_subset(map, test);
	isl_map_free(map);
	isl_map_free(test);

	return local;
}

/* Does "graph" have any satisfied condition edges that
 * are adjacent to the conditional validity constraint with
 * domain "conditional_source" and range "conditional_sink"?
 *
 * A satisfied condition is one that is not local.
 * If a condition was forced to be local already (i.e., marked as local)
 * then there is no need to check if it is in fact local.
 *
 * Additionally, mark all adjacent condition edges found as local.
 */
static int has_adjacent_true_conditions(struct isl_sched_graph *graph,
	__isl_keep isl_union_set *conditional_source,
	__isl_keep isl_union_set *conditional_sink)
{
	int i;
	int any = 0;

	for (i = 0; i < graph->n_edge; ++i) {
		int adjacent, local;
		isl_union_map *condition;

		if (!graph->edge[i].condition)
			continue;
		if (graph->edge[i].local)
			continue;

		condition = graph->edge[i].tagged_condition;
		adjacent = domain_intersects(condition, conditional_sink);
		if (adjacent >= 0 && !adjacent)
			adjacent = range_intersects(condition,
							conditional_source);
		if (adjacent < 0)
			return -1;
		if (!adjacent)
			continue;

		graph->edge[i].local = 1;

		local = is_condition_false(&graph->edge[i]);
		if (local < 0)
			return -1;
		if (!local)
			any = 1;
	}

	return any;
}

/* Are there any violated conditional validity dependences with
 * adjacent condition dependences that are not local with respect
 * to the current schedule?
 * That is, is the conditional validity constraint violated?
 *
 * Additionally, mark all those adjacent condition dependences as local.
 * We also mark those adjacent condition dependences that were not marked
 * as local before, but just happened to be local already.  This ensures
 * that they remain local if the schedule is recomputed.
 *
 * We first collect domain and range of all violated conditional validity
 * dependences and then check if there are any adjacent non-local
 * condition dependences.
 */
static int has_violated_conditional_constraint(isl_ctx *ctx,
	struct isl_sched_graph *graph)
{
	int i;
	int any = 0;
	isl_union_set *source, *sink;

	source = isl_union_set_empty(isl_space_params_alloc(ctx, 0));
	sink = isl_union_set_empty(isl_space_params_alloc(ctx, 0));
	for (i = 0; i < graph->n_edge; ++i) {
		isl_union_set *uset;
		isl_union_map *umap;
		int violated;

		if (!graph->edge[i].conditional_validity)
			continue;

		violated = is_violated(graph, i);
		if (violated < 0)
			goto error;
		if (!violated)
			continue;

		any = 1;

		umap = isl_union_map_copy(graph->edge[i].tagged_validity);
		uset = isl_union_map_domain(umap);
		source = isl_union_set_union(source, uset);
		source = isl_union_set_coalesce(source);

		umap = isl_union_map_copy(graph->edge[i].tagged_validity);
		uset = isl_union_map_range(umap);
		sink = isl_union_set_union(sink, uset);
		sink = isl_union_set_coalesce(sink);
	}

	if (any)
		any = has_adjacent_true_conditions(graph, source, sink);

	isl_union_set_free(source);
	isl_union_set_free(sink);
	return any;
error:
	isl_union_set_free(source);
	isl_union_set_free(sink);
	return -1;
}

/* Compute a schedule for a connected dependence graph.
 * We try to find a sequence of as many schedule rows as possible that result
 * in non-negative dependence distances (independent of the previous rows
 * in the sequence, i.e., such that the sequence is tilable), with as
 * many of the initial rows as possible satisfying the coincidence constraints.
 * If we can't find any more rows we either
 * - split between SCCs and start over (assuming we found an interesting
 *	pair of SCCs between which to split)
 * - continue with the next band (assuming the current band has at least
 *	one row)
 * - try to carry as many dependences as possible and continue with the next
 *	band
 *
 * If Feautrier's algorithm is selected, we first recursively try to satisfy
 * as many validity dependences as possible. When all validity dependences
 * are satisfied we extend the schedule to a full-dimensional schedule.
 *
 * If we manage to complete the schedule, we finish off by topologically
 * sorting the statements based on the remaining dependences.
 *
 * If ctx->opt->schedule_outer_coincidence is set, then we force the
 * outermost dimension to satisfy the coincidence constraints.  If this
 * turns out to be impossible, we fall back on the general scheme above
 * and try to carry as many dependences as possible.
 *
 * If "graph" contains both condition and conditional validity dependences,
 * then we need to check that that the conditional schedule constraint
 * is satisfied, i.e., there are no violated conditional validity dependences
 * that are adjacent to any non-local condition dependences.
 * If there are, then we mark all those adjacent condition dependences
 * as local and recompute the current band.  Those dependences that
 * are marked local will then be forced to be local.
 * The initial computation is performed with no dependences marked as local.
 * If we are lucky, then there will be no violated conditional validity
 * dependences adjacent to any non-local condition dependences.
 * Otherwise, we mark some additional condition dependences as local and
 * recompute.  We continue this process until there are no violations left or
 * until we are no longer able to compute a schedule.
 * Since there are only a finite number of dependences,
 * there will only be a finite number of iterations.
 */
static int compute_schedule_wcc(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int has_coincidence;
	int use_coincidence;
	int force_coincidence = 0;
	int check_conditional;

	if (detect_sccs(ctx, graph) < 0)
		return -1;
	if (sort_sccs(graph) < 0)
		return -1;

	if (compute_maxvar(graph) < 0)
		return -1;

	if (need_feautrier_step(ctx, graph))
		return compute_schedule_wcc_feautrier(ctx, graph);

	clear_local_edges(graph);
	check_conditional = need_condition_check(graph);
	has_coincidence = has_any_coincidence(graph);

	if (ctx->opt->schedule_outer_coincidence)
		force_coincidence = 1;

	use_coincidence = has_coincidence;
	while (graph->n_row < graph->maxvar) {
		isl_vec *sol;
		int violated;
		int coincident;

		graph->src_scc = -1;
		graph->dst_scc = -1;

		if (setup_lp(ctx, graph, use_coincidence) < 0)
			return -1;
		sol = solve_lp(graph);
		if (!sol)
			return -1;
		if (sol->size == 0) {
			int empty = graph->n_total_row == graph->band_start;

			isl_vec_free(sol);
			if (use_coincidence && (!force_coincidence || !empty)) {
				use_coincidence = 0;
				continue;
			}
			if (!ctx->opt->schedule_maximize_band_depth && !empty)
				return compute_next_band(ctx, graph);
			if (graph->src_scc >= 0)
				return compute_split_schedule(ctx, graph);
			if (!empty)
				return compute_next_band(ctx, graph);
			return carry_dependences(ctx, graph);
		}
		coincident = !has_coincidence || use_coincidence;
		if (update_schedule(graph, sol, 1, coincident) < 0)
			return -1;

		if (!check_conditional)
			continue;
		violated = has_violated_conditional_constraint(ctx, graph);
		if (violated < 0)
			return -1;
		if (!violated)
			continue;
		if (reset_band(graph) < 0)
			return -1;
		use_coincidence = has_coincidence;
	}

	if (graph->n_total_row > graph->band_start)
		next_band(graph);
	return sort_statements(ctx, graph);
}

/* Add a row to the schedules that separates the SCCs and move
 * to the next band.
 */
static int split_on_scc(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	int i;

	if (graph->n_total_row >= graph->max_row)
		isl_die(ctx, isl_error_internal,
			"too many schedule rows", return -1);

	for (i = 0; i < graph->n; ++i) {
		struct isl_sched_node *node = &graph->node[i];
		int row = isl_mat_rows(node->sched);

		isl_map_free(node->sched_map);
		node->sched_map = NULL;
		node->sched = isl_mat_add_zero_rows(node->sched, 1);
		node->sched = isl_mat_set_element_si(node->sched, row, 0,
						     node->scc);
		if (!node->sched)
			return -1;
		node->band[graph->n_total_row] = graph->n_band;
	}

	graph->n_total_row++;
	next_band(graph);

	return 0;
}

/* Compute a schedule for each component (identified by node->scc)
 * of the dependence graph separately and then combine the results.
 * Depending on the setting of schedule_fuse, a component may be
 * either weakly or strongly connected.
 *
 * The band_id is adjusted such that each component has a separate id.
 * Note that the band_id may have already been set to a value different
 * from zero by compute_split_schedule.
 */
static int compute_component_schedule(isl_ctx *ctx,
	struct isl_sched_graph *graph)
{
	int wcc, i;
	int n, n_edge;
	int n_total_row, orig_total_row;
	int n_band, orig_band;

	if (ctx->opt->schedule_fuse == ISL_SCHEDULE_FUSE_MIN ||
	    ctx->opt->schedule_separate_components)
		if (split_on_scc(ctx, graph) < 0)
			return -1;

	n_total_row = 0;
	orig_total_row = graph->n_total_row;
	n_band = 0;
	orig_band = graph->n_band;
	for (i = 0; i < graph->n; ++i)
		graph->node[i].band_id[graph->n_band] += graph->node[i].scc;
	for (wcc = 0; wcc < graph->scc; ++wcc) {
		n = 0;
		for (i = 0; i < graph->n; ++i)
			if (graph->node[i].scc == wcc)
				n++;
		n_edge = 0;
		for (i = 0; i < graph->n_edge; ++i)
			if (graph->edge[i].src->scc == wcc &&
			    graph->edge[i].dst->scc == wcc)
				n_edge++;

		if (compute_sub_schedule(ctx, graph, n, n_edge,
				    &node_scc_exactly,
				    &edge_scc_exactly, wcc, 1) < 0)
			return -1;
		if (graph->n_total_row > n_total_row)
			n_total_row = graph->n_total_row;
		graph->n_total_row = orig_total_row;
		if (graph->n_band > n_band)
			n_band = graph->n_band;
		graph->n_band = orig_band;
	}

	graph->n_total_row = n_total_row;
	graph->n_band = n_band;

	return pad_schedule(graph);
}

/* Compute a schedule for the given dependence graph.
 * We first check if the graph is connected (through validity and conditional
 * validity dependences) and, if not, compute a schedule
 * for each component separately.
 * If schedule_fuse is set to minimal fusion, then we check for strongly
 * connected components instead and compute a separate schedule for
 * each such strongly connected component.
 */
static int compute_schedule(isl_ctx *ctx, struct isl_sched_graph *graph)
{
	if (ctx->opt->schedule_fuse == ISL_SCHEDULE_FUSE_MIN) {
		if (detect_sccs(ctx, graph) < 0)
			return -1;
	} else {
		if (detect_wccs(ctx, graph) < 0)
			return -1;
	}

	if (graph->scc > 1)
		return compute_component_schedule(ctx, graph);

	return compute_schedule_wcc(ctx, graph);
}

/* Compute a schedule on sc->domain that respects the given schedule
 * constraints.
 *
 * In particular, the schedule respects all the validity dependences.
 * If the default isl scheduling algorithm is used, it tries to minimize
 * the dependence distances over the proximity dependences.
 * If Feautrier's scheduling algorithm is used, the proximity dependence
 * distances are only minimized during the extension to a full-dimensional
 * schedule.
 *
 * If there are any condition and conditional validity dependences,
 * then the conditional validity dependences may be violated inside
 * a tilable band, provided they have no adjacent non-local
 * condition dependences.
 */
__isl_give isl_schedule *isl_schedule_constraints_compute_schedule(
	__isl_take isl_schedule_constraints *sc)
{
	isl_ctx *ctx = isl_schedule_constraints_get_ctx(sc);
	struct isl_sched_graph graph = { 0 };
	isl_schedule *sched;
	struct isl_extract_edge_data data;
	enum isl_edge_type i;

	sc = isl_schedule_constraints_align_params(sc);
	if (!sc)
		return NULL;

	graph.n = isl_union_set_n_set(sc->domain);
	if (graph.n == 0)
		goto empty;
	if (graph_alloc(ctx, &graph, graph.n,
	    isl_schedule_constraints_n_map(sc)) < 0)
		goto error;
	if (compute_max_row(&graph, sc->domain) < 0)
		goto error;
	graph.root = 1;
	graph.n = 0;
	if (isl_union_set_foreach_set(sc->domain, &extract_node, &graph) < 0)
		goto error;
	if (graph_init_table(ctx, &graph) < 0)
		goto error;
	for (i = isl_edge_first; i <= isl_edge_last; ++i)
		graph.max_edge[i] = isl_union_map_n_map(sc->constraint[i]);
	if (graph_init_edge_tables(ctx, &graph) < 0)
		goto error;
	graph.n_edge = 0;
	data.graph = &graph;
	for (i = isl_edge_first; i <= isl_edge_last; ++i) {
		data.type = i;
		if (isl_union_map_foreach_map(sc->constraint[i],
						&extract_edge, &data) < 0)
			goto error;
	}

	if (compute_schedule(ctx, &graph) < 0)
		goto error;

empty:
	sched = extract_schedule(&graph, isl_union_set_get_space(sc->domain));

	graph_free(ctx, &graph);
	isl_schedule_constraints_free(sc);

	return sched;
error:
	graph_free(ctx, &graph);
	isl_schedule_constraints_free(sc);
	return NULL;
}

/* Compute a schedule for the given union of domains that respects
 * all the validity dependences and minimizes
 * the dependence distances over the proximity dependences.
 *
 * This function is kept for backward compatibility.
 */
__isl_give isl_schedule *isl_union_set_compute_schedule(
	__isl_take isl_union_set *domain,
	__isl_take isl_union_map *validity,
	__isl_take isl_union_map *proximity)
{
	isl_schedule_constraints *sc;

	sc = isl_schedule_constraints_on_domain(domain);
	sc = isl_schedule_constraints_set_validity(sc, validity);
	sc = isl_schedule_constraints_set_proximity(sc, proximity);

	return isl_schedule_constraints_compute_schedule(sc);
}

__isl_null isl_schedule *isl_schedule_free(__isl_take isl_schedule *sched)
{
	int i;
	if (!sched)
		return NULL;

	if (--sched->ref > 0)
		return NULL;

	for (i = 0; i < sched->n; ++i) {
		isl_multi_aff_free(sched->node[i].sched);
		free(sched->node[i].band_end);
		free(sched->node[i].band_id);
		free(sched->node[i].coincident);
	}
	isl_space_free(sched->dim);
	isl_band_list_free(sched->band_forest);
	free(sched);
	return NULL;
}

isl_ctx *isl_schedule_get_ctx(__isl_keep isl_schedule *schedule)
{
	return schedule ? isl_space_get_ctx(schedule->dim) : NULL;
}

/* Set max_out to the maximal number of output dimensions over
 * all maps.
 */
static int update_max_out(__isl_take isl_map *map, void *user)
{
	int *max_out = user;
	int n_out = isl_map_dim(map, isl_dim_out);

	if (n_out > *max_out)
		*max_out = n_out;

	isl_map_free(map);
	return 0;
}

/* Internal data structure for map_pad_range.
 *
 * "max_out" is the maximal schedule dimension.
 * "res" collects the results.
 */
struct isl_pad_schedule_map_data {
	int max_out;
	isl_union_map *res;
};

/* Pad the range of the given map with zeros to data->max_out and
 * then add the result to data->res.
 */
static int map_pad_range(__isl_take isl_map *map, void *user)
{
	struct isl_pad_schedule_map_data *data = user;
	int i;
	int n_out = isl_map_dim(map, isl_dim_out);

	map = isl_map_add_dims(map, isl_dim_out, data->max_out - n_out);
	for (i = n_out; i < data->max_out; ++i)
		map = isl_map_fix_si(map, isl_dim_out, i, 0);

	data->res = isl_union_map_add_map(data->res, map);
	if (!data->res)
		return -1;

	return 0;
}

/* Pad the ranges of the maps in the union map with zeros such they all have
 * the same dimension.
 */
static __isl_give isl_union_map *pad_schedule_map(
	__isl_take isl_union_map *umap)
{
	struct isl_pad_schedule_map_data data;

	if (!umap)
		return NULL;
	if (isl_union_map_n_map(umap) <= 1)
		return umap;

	data.max_out = 0;
	if (isl_union_map_foreach_map(umap, &update_max_out, &data.max_out) < 0)
		return isl_union_map_free(umap);

	data.res = isl_union_map_empty(isl_union_map_get_space(umap));
	if (isl_union_map_foreach_map(umap, &map_pad_range, &data) < 0)
		data.res = isl_union_map_free(data.res);

	isl_union_map_free(umap);
	return data.res;
}

/* Return an isl_union_map of the schedule.  If we have already constructed
 * a band forest, then this band forest may have been modified so we need
 * to extract the isl_union_map from the forest rather than from
 * the originally computed schedule.  This reconstructed schedule map
 * then needs to be padded with zeros to unify the schedule space
 * since the result of isl_band_list_get_suffix_schedule may not have
 * a unified schedule space.
 */
__isl_give isl_union_map *isl_schedule_get_map(__isl_keep isl_schedule *sched)
{
	int i;
	isl_union_map *umap;

	if (!sched)
		return NULL;

	if (sched->band_forest) {
		umap = isl_band_list_get_suffix_schedule(sched->band_forest);
		return pad_schedule_map(umap);
	}

	umap = isl_union_map_empty(isl_space_copy(sched->dim));
	for (i = 0; i < sched->n; ++i) {
		isl_multi_aff *ma;

		ma = isl_multi_aff_copy(sched->node[i].sched);
		umap = isl_union_map_add_map(umap, isl_map_from_multi_aff(ma));
	}

	return umap;
}

static __isl_give isl_band_list *construct_band_list(
	__isl_keep isl_schedule *schedule, __isl_keep isl_band *parent,
	int band_nr, int *parent_active, int n_active);

/* Construct an isl_band structure for the band in the given schedule
 * with sequence number band_nr for the n_active nodes marked by active.
 * If the nodes don't have a band with the given sequence number,
 * then a band without members is created.
 *
 * Because of the way the schedule is constructed, we know that
 * the position of the band inside the schedule of a node is the same
 * for all active nodes.
 *
 * The partial schedule for the band is created before the children
 * are created to that construct_band_list can refer to the partial
 * schedule of the parent.
 */
static __isl_give isl_band *construct_band(__isl_keep isl_schedule *schedule,
	__isl_keep isl_band *parent,
	int band_nr, int *active, int n_active)
{
	int i, j;
	isl_ctx *ctx = isl_schedule_get_ctx(schedule);
	isl_band *band;
	unsigned start, end;

	band = isl_band_alloc(ctx);
	if (!band)
		return NULL;

	band->schedule = schedule;
	band->parent = parent;

	for (i = 0; i < schedule->n; ++i)
		if (active[i])
			break;

	if (i >= schedule->n)
		isl_die(ctx, isl_error_internal,
			"band without active statements", goto error);

	start = band_nr ? schedule->node[i].band_end[band_nr - 1] : 0;
	end = band_nr < schedule->node[i].n_band ?
		schedule->node[i].band_end[band_nr] : start;
	band->n = end - start;

	band->coincident = isl_alloc_array(ctx, int, band->n);
	if (band->n && !band->coincident)
		goto error;

	for (j = 0; j < band->n; ++j)
		band->coincident[j] = schedule->node[i].coincident[start + j];

	band->pma = isl_union_pw_multi_aff_empty(isl_space_copy(schedule->dim));
	for (i = 0; i < schedule->n; ++i) {
		isl_multi_aff *ma;
		isl_pw_multi_aff *pma;
		unsigned n_out;

		if (!active[i])
			continue;

		ma = isl_multi_aff_copy(schedule->node[i].sched);
		n_out = isl_multi_aff_dim(ma, isl_dim_out);
		ma = isl_multi_aff_drop_dims(ma, isl_dim_out, end, n_out - end);
		ma = isl_multi_aff_drop_dims(ma, isl_dim_out, 0, start);
		pma = isl_pw_multi_aff_from_multi_aff(ma);
		band->pma = isl_union_pw_multi_aff_add_pw_multi_aff(band->pma,
								    pma);
	}
	if (!band->pma)
		goto error;

	for (i = 0; i < schedule->n; ++i)
		if (active[i] && schedule->node[i].n_band > band_nr + 1)
			break;

	if (i < schedule->n) {
		band->children = construct_band_list(schedule, band,
						band_nr + 1, active, n_active);
		if (!band->children)
			goto error;
	}

	return band;
error:
	isl_band_free(band);
	return NULL;
}

/* Internal data structure used inside cmp_band and pw_multi_aff_extract_int.
 *
 * r is set to a negative value if anything goes wrong.
 *
 * c1 stores the result of extract_int.
 * c2 is a temporary value used inside cmp_band_in_ancestor.
 * t is a temporary value used inside extract_int.
 *
 * first and equal are used inside extract_int.
 * first is set if we are looking at the first isl_multi_aff inside
 * the isl_union_pw_multi_aff.
 * equal is set if all the isl_multi_affs have been equal so far.
 */
struct isl_cmp_band_data {
	int r;

	int first;
	int equal;

	isl_int t;
	isl_int c1;
	isl_int c2;
};

/* Check if "ma" assigns a constant value.
 * Note that this function is only called on isl_multi_affs
 * with a single output dimension.
 *
 * If "ma" assigns a constant value then we compare it to data->c1
 * or assign it to data->c1 if this is the first isl_multi_aff we consider.
 * If "ma" does not assign a constant value or if it assigns a value
 * that is different from data->c1, then we set data->equal to zero
 * and terminate the check.
 */
static int multi_aff_extract_int(__isl_take isl_set *set,
	__isl_take isl_multi_aff *ma, void *user)
{
	isl_aff *aff;
	struct isl_cmp_band_data *data = user;

	aff = isl_multi_aff_get_aff(ma, 0);
	data->r = isl_aff_is_cst(aff);
	if (data->r >= 0 && data->r) {
		isl_aff_get_constant(aff, &data->t);
		if (data->first) {
			isl_int_set(data->c1, data->t);
			data->first = 0;
		} else if (!isl_int_eq(data->c1, data->t))
			data->equal = 0;
	} else if (data->r >= 0 && !data->r)
		data->equal = 0;

	isl_aff_free(aff);
	isl_set_free(set);
	isl_multi_aff_free(ma);

	if (data->r < 0)
		return -1;
	if (!data->equal)
		return -1;
	return 0;
}

/* This function is called for each isl_pw_multi_aff in
 * the isl_union_pw_multi_aff checked by extract_int.
 * Check all the isl_multi_affs inside "pma".
 */
static int pw_multi_aff_extract_int(__isl_take isl_pw_multi_aff *pma,
	void *user)
{
	int r;

	r = isl_pw_multi_aff_foreach_piece(pma, &multi_aff_extract_int, user);
	isl_pw_multi_aff_free(pma);

	return r;
}

/* Check if "upma" assigns a single constant value to its domain.
 * If so, return 1 and store the result in data->c1.
 * If not, return 0.
 *
 * A negative return value from isl_union_pw_multi_aff_foreach_pw_multi_aff
 * means that either an error occurred or that we have broken off the check
 * because we already know the result is going to be negative.
 * In the latter case, data->equal is set to zero.
 */
static int extract_int(__isl_keep isl_union_pw_multi_aff *upma,
	struct isl_cmp_band_data *data)
{
	data->first = 1;
	data->equal = 1;

	if (isl_union_pw_multi_aff_foreach_pw_multi_aff(upma,
					&pw_multi_aff_extract_int, data) < 0) {
		if (!data->equal)
			return 0;
		return -1;
	}

	return !data->first && data->equal;
}

/* Compare "b1" and "b2" based on the parent schedule of their ancestor
 * "ancestor".
 *
 * If the parent of "ancestor" also has a single member, then we
 * first try to compare the two band based on the partial schedule
 * of this parent.
 *
 * Otherwise, or if the result is inconclusive, we look at the partial schedule
 * of "ancestor" itself.
 * In particular, we specialize the parent schedule based
 * on the domains of the child schedules, check if both assign
 * a single constant value and, if so, compare the two constant values.
 * If the specialized parent schedules do not assign a constant value,
 * then they cannot be used to order the two bands and so in this case
 * we return 0.
 */
static int cmp_band_in_ancestor(__isl_keep isl_band *b1,
	__isl_keep isl_band *b2, struct isl_cmp_band_data *data,
	__isl_keep isl_band *ancestor)
{
	isl_union_pw_multi_aff *upma;
	isl_union_set *domain;
	int r;

	if (data->r < 0)
		return 0;

	if (ancestor->parent && ancestor->parent->n == 1) {
		r = cmp_band_in_ancestor(b1, b2, data, ancestor->parent);
		if (data->r < 0)
			return 0;
		if (r)
			return r;
	}

	upma = isl_union_pw_multi_aff_copy(b1->pma);
	domain = isl_union_pw_multi_aff_domain(upma);
	upma = isl_union_pw_multi_aff_copy(ancestor->pma);
	upma = isl_union_pw_multi_aff_intersect_domain(upma, domain);
	r = extract_int(upma, data);
	isl_union_pw_multi_aff_free(upma);

	if (r < 0)
		data->r = -1;
	if (r < 0 || !r)
		return 0;

	isl_int_set(data->c2, data->c1);

	upma = isl_union_pw_multi_aff_copy(b2->pma);
	domain = isl_union_pw_multi_aff_domain(upma);
	upma = isl_union_pw_multi_aff_copy(ancestor->pma);
	upma = isl_union_pw_multi_aff_intersect_domain(upma, domain);
	r = extract_int(upma, data);
	isl_union_pw_multi_aff_free(upma);

	if (r < 0)
		data->r = -1;
	if (r < 0 || !r)
		return 0;

	return isl_int_cmp(data->c2, data->c1);
}

/* Compare "a" and "b" based on the parent schedule of their parent.
 */
static int cmp_band(const void *a, const void *b, void *user)
{
	isl_band *b1 = *(isl_band * const *) a;
	isl_band *b2 = *(isl_band * const *) b;
	struct isl_cmp_band_data *data = user;

	return cmp_band_in_ancestor(b1, b2, data, b1->parent);
}

/* Sort the elements in "list" based on the partial schedules of its parent
 * (and ancestors).  In particular if the parent assigns constant values
 * to the domains of the bands in "list", then the elements are sorted
 * according to that order.
 * This order should be a more "natural" order for the user, but otherwise
 * shouldn't have any effect.
 * If we would be constructing an isl_band forest directly in
 * isl_schedule_constraints_compute_schedule then there wouldn't be any need
 * for a reordering, since the children would be added to the list
 * in their natural order automatically.
 *
 * If there is only one element in the list, then there is no need to sort
 * anything.
 * If the partial schedule of the parent has more than one member
 * (or if there is no parent), then it's
 * defnitely not assigning constant values to the different children in
 * the list and so we wouldn't be able to use it to sort the list.
 */
static __isl_give isl_band_list *sort_band_list(__isl_take isl_band_list *list,
	__isl_keep isl_band *parent)
{
	struct isl_cmp_band_data data;

	if (!list)
		return NULL;
	if (list->n <= 1)
		return list;
	if (!parent || parent->n != 1)
		return list;

	data.r = 0;
	isl_int_init(data.c1);
	isl_int_init(data.c2);
	isl_int_init(data.t);
	isl_sort(list->p, list->n, sizeof(list->p[0]), &cmp_band, &data);
	if (data.r < 0)
		list = isl_band_list_free(list);
	isl_int_clear(data.c1);
	isl_int_clear(data.c2);
	isl_int_clear(data.t);

	return list;
}

/* Construct a list of bands that start at the same position (with
 * sequence number band_nr) in the schedules of the nodes that
 * were active in the parent band.
 *
 * A separate isl_band structure is created for each band_id
 * and for each node that does not have a band with sequence
 * number band_nr.  In the latter case, a band without members
 * is created.
 * This ensures that if a band has any children, then each node
 * that was active in the band is active in exactly one of the children.
 */
static __isl_give isl_band_list *construct_band_list(
	__isl_keep isl_schedule *schedule, __isl_keep isl_band *parent,
	int band_nr, int *parent_active, int n_active)
{
	int i, j;
	isl_ctx *ctx = isl_schedule_get_ctx(schedule);
	int *active;
	int n_band;
	isl_band_list *list;

	n_band = 0;
	for (i = 0; i < n_active; ++i) {
		for (j = 0; j < schedule->n; ++j) {
			if (!parent_active[j])
				continue;
			if (schedule->node[j].n_band <= band_nr)
				continue;
			if (schedule->node[j].band_id[band_nr] == i) {
				n_band++;
				break;
			}
		}
	}
	for (j = 0; j < schedule->n; ++j)
		if (schedule->node[j].n_band <= band_nr)
			n_band++;

	if (n_band == 1) {
		isl_band *band;
		list = isl_band_list_alloc(ctx, n_band);
		band = construct_band(schedule, parent, band_nr,
					parent_active, n_active);
		return isl_band_list_add(list, band);
	}

	active = isl_alloc_array(ctx, int, schedule->n);
	if (schedule->n && !active)
		return NULL;

	list = isl_band_list_alloc(ctx, n_band);

	for (i = 0; i < n_active; ++i) {
		int n = 0;
		isl_band *band;

		for (j = 0; j < schedule->n; ++j) {
			active[j] = parent_active[j] &&
					schedule->node[j].n_band > band_nr &&
					schedule->node[j].band_id[band_nr] == i;
			if (active[j])
				n++;
		}
		if (n == 0)
			continue;

		band = construct_band(schedule, parent, band_nr, active, n);

		list = isl_band_list_add(list, band);
	}
	for (i = 0; i < schedule->n; ++i) {
		isl_band *band;
		if (!parent_active[i])
			continue;
		if (schedule->node[i].n_band > band_nr)
			continue;
		for (j = 0; j < schedule->n; ++j)
			active[j] = j == i;
		band = construct_band(schedule, parent, band_nr, active, 1);
		list = isl_band_list_add(list, band);
	}

	free(active);

	list = sort_band_list(list, parent);

	return list;
}

/* Construct a band forest representation of the schedule and
 * return the list of roots.
 */
static __isl_give isl_band_list *construct_forest(
	__isl_keep isl_schedule *schedule)
{
	int i;
	isl_ctx *ctx = isl_schedule_get_ctx(schedule);
	isl_band_list *forest;
	int *active;

	active = isl_alloc_array(ctx, int, schedule->n);
	if (schedule->n && !active)
		return NULL;

	for (i = 0; i < schedule->n; ++i)
		active[i] = 1;

	forest = construct_band_list(schedule, NULL, 0, active, schedule->n);

	free(active);

	return forest;
}

/* Return the roots of a band forest representation of the schedule.
 */
__isl_give isl_band_list *isl_schedule_get_band_forest(
	__isl_keep isl_schedule *schedule)
{
	if (!schedule)
		return NULL;
	if (!schedule->band_forest)
		schedule->band_forest = construct_forest(schedule);
	return isl_band_list_dup(schedule->band_forest);
}

/* Call "fn" on each band in the schedule in depth-first post-order.
 */
int isl_schedule_foreach_band(__isl_keep isl_schedule *sched,
	int (*fn)(__isl_keep isl_band *band, void *user), void *user)
{
	int r;
	isl_band_list *forest;

	if (!sched)
		return -1;

	forest = isl_schedule_get_band_forest(sched);
	r = isl_band_list_foreach_band(forest, fn, user);
	isl_band_list_free(forest);

	return r;
}

static __isl_give isl_printer *print_band_list(__isl_take isl_printer *p,
	__isl_keep isl_band_list *list);

static __isl_give isl_printer *print_band(__isl_take isl_printer *p,
	__isl_keep isl_band *band)
{
	isl_band_list *children;

	p = isl_printer_start_line(p);
	p = isl_printer_print_union_pw_multi_aff(p, band->pma);
	p = isl_printer_end_line(p);

	if (!isl_band_has_children(band))
		return p;

	children = isl_band_get_children(band);

	p = isl_printer_indent(p, 4);
	p = print_band_list(p, children);
	p = isl_printer_indent(p, -4);

	isl_band_list_free(children);

	return p;
}

static __isl_give isl_printer *print_band_list(__isl_take isl_printer *p,
	__isl_keep isl_band_list *list)
{
	int i, n;

	n = isl_band_list_n_band(list);
	for (i = 0; i < n; ++i) {
		isl_band *band;
		band = isl_band_list_get_band(list, i);
		p = print_band(p, band);
		isl_band_free(band);
	}

	return p;
}

__isl_give isl_printer *isl_printer_print_schedule(__isl_take isl_printer *p,
	__isl_keep isl_schedule *schedule)
{
	isl_band_list *forest;

	forest = isl_schedule_get_band_forest(schedule);

	p = print_band_list(p, forest);

	isl_band_list_free(forest);

	return p;
}

void isl_schedule_dump(__isl_keep isl_schedule *schedule)
{
	isl_printer *printer;

	if (!schedule)
		return;

	printer = isl_printer_to_file(isl_schedule_get_ctx(schedule), stderr);
	printer = isl_printer_print_schedule(printer, schedule);

	isl_printer_free(printer);
}
