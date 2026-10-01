/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Part of an MPL-2.0 Graphviz layout plugin derived from iongraph.
 * See NOTICE.md for attribution. */

#include <graphviz/cgraph.h>
#include <graphviz/geom.h>
#include <graphviz/gvplugin.h>
#include <graphviz/gvplugin_layout.h>
#include <graphviz/types.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "ion_layout.h"

#ifndef POINTS_PER_INCH
#define POINTS_PER_INCH 72.0
#endif

// These helpers are exported by libgvc but are not currently declared in the
// public plugin headers Graphviz installs.
extern void common_init_node(node_t *n);
extern void common_init_edge(edge_t *e);
extern void gv_nodesize(node_t *n, int flip);
extern void gv_cleanup_node(node_t *n);
extern void gv_cleanup_edge(edge_t *e);
extern void dotneato_postprocess(graph_t *g);
extern void setEdgeType(graph_t *g, int dflt);
/* libgvc owns graph_init/graph_cleanup around these callbacks. Calling them
 * here leaks the first drawing and invalidates GD_cleanup during teardown. */
/* libgvc's "last finished layout phase". attach_attrs_and_arrows() only
 * scans splines (and declares the _hdraw_/_tdraw_ xdot attributes) when
 * State >= GVSPLINES; without this, -Txdot/-Tjson agxset a NULL symbol and
 * crash. Layout engines are responsible for advancing it. */
extern int State;
#define ION_GVSPLINES 1 /* GVSPLINES from lib/common/const.h */

#define ET_SPLINE (5 << 1)

typedef struct IonNodeIndex {
  Agrec_t header;
  size_t index;
} IonNodeIndex;

static size_t node_index(node_t *n) {
  return ((IonNodeIndex *)aggetrec(n, "IonNodeIndex", false))->index;
}

static void ion_layout(graph_t *g) {
  IonGraph ctx = {.graph = g};
  size_t node_count = 0;
  size_t edge_count = 0;

  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n))
    node_count++;
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) {
    for (edge_t *e = agfstout(g, n); e; e = agnxtout(g, e))
      edge_count++;
  }

  node_t **nodes = calloc(node_count ? node_count : 1, sizeof(node_t *));
  double *widths = calloc(node_count ? node_count : 1, sizeof(double));
  double *heights = calloc(node_count ? node_count : 1, sizeof(double));
  int *loop_depths = calloc(node_count ? node_count : 1, sizeof(int));
  unsigned char *loop_headers =
      calloc(node_count ? node_count : 1, sizeof(unsigned char));
  unsigned char *backedges =
      calloc(node_count ? node_count : 1, sizeof(unsigned char));
  IonPoint *positions = calloc(node_count ? node_count : 1, sizeof(IonPoint));
  IonRoute *routes = calloc(edge_count ? edge_count : 1, sizeof(IonRoute));
  size_t *tails = calloc(edge_count ? edge_count : 1, sizeof(size_t));
  size_t *heads = calloc(edge_count ? edge_count : 1, sizeof(size_t));
  edge_t **ordered_edges =
      calloc(edge_count ? edge_count : 1, sizeof(edge_t *));

  if (!nodes || !widths || !heights || !loop_depths || !loop_headers ||
      !backedges || !positions || !routes || !tails || !heads || !ordered_edges)
    goto done;

  size_t i = 0;
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n), i++) {
    agbindrec(n, "Agnodeinfo_t", sizeof(Agnodeinfo_t), true);
    nodes[i] = n;
    ((IonNodeIndex *)agbindrec(n, "IonNodeIndex", sizeof(IonNodeIndex), false))
        ->index = i;
    common_init_node(n);
    gv_nodesize(n, 0);
    widths[i] = ND_width(n) * POINTS_PER_INCH;
    heights[i] = ND_height(n) * POINTS_PER_INCH;
    char *depth = agget(n, "ion_loop_depth");
    loop_depths[i] = depth ? atoi(depth) : 0;
    char *header = agget(n, "ion_loop_header");
    char *backedge = agget(n, "ion_backedge");
    loop_headers[i] =
        header && (strcmp(header, "true") == 0 || strcmp(header, "1") == 0);
    backedges[i] = backedge && (strcmp(backedge, "true") == 0 ||
                                strcmp(backedge, "1") == 0);
  }

  /* cgraph yields out-edges in declaration order; preserve that ordering
   * for Ion successor ports. */
  size_t edge_i = 0;
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) {
    for (edge_t *e = agfstout(g, n); e; e = agnxtout(g, e)) {
      agbindrec(e, "Agedgeinfo_t", sizeof(Agedgeinfo_t), true);
      common_init_edge(e);
      size_t tail = node_index(agtail(e));
      size_t head = node_index(aghead(e));
      if (tail < node_count && head < node_count) {
        tails[edge_i] = (size_t)tail;
        heads[edge_i] = (size_t)head;
        ordered_edges[edge_i] = e;
        edge_i++;
      }
    }
  }

  ctx.node_count = node_count;
  ctx.edge_count = edge_i;
  ctx.nodes = nodes;
  ctx.edges = ordered_edges;
  ion_reserve_incoming(&ctx, widths, heights);
  if (ion_extract_constraints(&ctx))
    goto done;
  IonOptions options = {.nodesep = -1,
                        .ranksep = -1,
                        .arrow_clearance = ion_arrow_clearance(&ctx)};
  char *sep = agget(g, "nodesep");
  if (sep && *sep) {
    char *end;
    double v = strtod(sep, &end);
    if (end != sep && isfinite(v))
      options.nodesep = fmax(0.02, v) * POINTS_PER_INCH;
  }
  sep = agget(g, "ranksep");
  if (sep && *sep) {
    char *end;
    double v = strtod(sep, &end);
    if (end != sep && isfinite(v))
      options.ranksep = fmax(0.02, v) * POINTS_PER_INCH;
    options.ranksep_equally = strstr(sep, "equally") != NULL;
  }
  unsigned int orientation = ION_TOP_TO_BOTTOM;
  char *rankdir = agget(g, "rankdir");
  if (rankdir) {
    if (strcmp(rankdir, "LR") == 0)
      orientation = ION_LEFT_TO_RIGHT;
    else if (strcmp(rankdir, "BT") == 0)
      orientation = ION_BOTTOM_TO_TOP;
    else if (strcmp(rankdir, "RL") == 0)
      orientation = ION_RIGHT_TO_LEFT;
  }

  if (getenv("ION_DUMP_INPUT")) {
    for (size_t k = 0; k < node_count; k++)
      fprintf(stderr, "node %.17g %.17g %d %d %d\n", widths[k], heights[k],
              loop_depths[k], loop_headers[k], backedges[k]);
    for (size_t k = 0; k < edge_i; k++)
      fprintf(stderr, "edge %zu %zu\n", tails[k], heads[k]);
  }

  double graph_width = 0.0;
  double graph_height = 0.0;
  IonPoint *route_points = NULL;
  size_t route_points_len = 0;
  if (ion_layout_compute_ex(node_count, widths, heights, edge_i, tails, heads,
                            loop_depths, loop_headers, backedges, orientation,
                            positions, routes, &route_points, &route_points_len,
                            &graph_width, &graph_height, ctx.node_constraints,
                            ctx.edge_constraints, ctx.clusters,
                            ctx.cluster_count, &options) != 0) {
    goto done;
  }

  setEdgeType(g, ET_SPLINE);
  for (i = 0; i < node_count; i++) {
    node_t *n = nodes[i];
    // widths[i]/heights[i] are now the EXPANDED cell sizes written back
    // by ion_layout_compute (e.g. nodes widened to fit their output
    // ports). Setting ND_width alone is not enough: the node's shape
    // POLYGON was already built by common_init_node from the label size,
    // and renderers draw that polygon. Re-initialize the shape with the
    // layout's size as a minimum via the width/height attributes.
    double grow_w = widths[i] - ND_width(n) * POINTS_PER_INCH;
    double grow_h = heights[i] - ND_height(n) * POINTS_PER_INCH;
    if (grow_w > 0.01 || grow_h > 0.01) {
      /* poly_init sizes non-regular shapes from ND_width/ND_height
       * (maxed against the label), so set those first, then rebuild
       * the shape polygon. */
      ND_width(n) = widths[i] / POINTS_PER_INCH;
      ND_height(n) = heights[i] / POINTS_PER_INCH;
      if (ND_shape(n) && ND_shape(n)->fns) {
        if (ND_shape(n)->fns->freefn)
          ND_shape(n)->fns->freefn(n);
        if (ND_shape(n)->fns->initfn)
          ND_shape(n)->fns->initfn(n);
      }
    }
    ND_pos(n) = calloc(2, sizeof(double));
    if (!ND_pos(n)) {
      ion_layout_free_points(route_points, route_points_len);
      goto done;
    }
    ND_width(n) = widths[i] / POINTS_PER_INCH;
    ND_height(n) = heights[i] / POINTS_PER_INCH;
    ND_lw(n) = widths[i] / 2.0;
    ND_rw(n) = widths[i] / 2.0;
    ND_ht(n) = heights[i];
    ND_coord(n).x = positions[i].x;
    ND_coord(n).y = graph_height - positions[i].y;
    ND_pos(n)[0] = ND_coord(n).x / POINTS_PER_INCH;
    ND_pos(n)[1] = ND_coord(n).y / POINTS_PER_INCH;
    ND_bb(n).LL.x = ND_coord(n).x - widths[i] / 2.0;
    ND_bb(n).LL.y = ND_coord(n).y - heights[i] / 2.0;
    ND_bb(n).UR.x = ND_coord(n).x + widths[i] / 2.0;
    ND_bb(n).UR.y = ND_coord(n).y + heights[i] / 2.0;
  }

  GD_bb(g).LL.x = 0.0;
  GD_bb(g).LL.y = 0.0;
  GD_bb(g).UR.x = graph_width;
  GD_bb(g).UR.y = graph_height;

  ion_install_clusters(&ctx, graph_height);
  for (size_t k = 0; k < edge_i; k++) {
    /* Shape rebuilding can replace record/HTML field boxes. Re-resolve
     * port attributes without recreating edge labels. */
    edge_t *e = ordered_edges[k];
    char *tp = agget(e, "tailport"), *hp = agget(e, "headport");
    char *port_attrs[2] = {tp, hp};
    node_t *end_nodes[2] = {agtail(e), aghead(e)};
    port *ports[2] = {&ED_tail_port(e), &ED_head_port(e)};
    for (int side = 0; side < 2; side++) {
      if (port_attrs[side] && *port_attrs[side]) {
        char *name = strdup(port_attrs[side]);
        if (!name)
          continue;
        char *compass = strchr(name, ':');
        if (compass)
          *compass++ = '\0';
        bool clip = ports[side]->clip;
        *ports[side] = ND_shape(end_nodes[side])
                           ->fns->portfn(end_nodes[side], name, compass);
        ports[side]->clip = clip;
        ports[side]->name = port_attrs[side];
        free(name);
      }
    }
    ion_route_edge(&ctx, k, routes[k], route_points, graph_height);
  }
  ion_layout_free_points(route_points, route_points_len);
  ion_place_labels(&ctx);
  State = ION_GVSPLINES;
  /* Graphviz translates nodes, splines, all edge labels and cluster boxes
   * together. Applying a second hand-written shift corrupts the geometry. */
  dotneato_postprocess(g);

done:
  ion_free_constraints(&ctx);
  free(ordered_edges);
  free(nodes);
  free(widths);
  free(heights);
  free(loop_depths);
  free(loop_headers);
  free(backedges);
  free(positions);
  free(routes);
  free(tails);
  free(heads);
}

static void ion_cleanup(graph_t *g) {
  for (node_t *n = agfstnode(g); n; n = agnxtnode(g, n)) {
    for (edge_t *e = agfstout(g, n); e; e = agnxtout(g, e))
      gv_cleanup_edge(e);
    gv_cleanup_node(n);
    agdelrec(n, "IonNodeIndex");
  }
  ion_cleanup_clusters(g);
}

static gvlayout_engine_t ion_engine = {
    ion_layout,
    ion_cleanup,
};

static gvlayout_features_t ion_features = {0};

static gvplugin_installed_t ion_layout_types[] = {
    {0, "ion", 0, &ion_engine, &ion_features},
    {0, NULL, 0, NULL, NULL},
};

static gvplugin_api_t ion_apis[] = {
    {API_layout, ion_layout_types},
    {(api_t)0, NULL},
};

gvplugin_library_t gvplugin_ion_LTX_library = {
    "ion",
    ion_apis,
};
