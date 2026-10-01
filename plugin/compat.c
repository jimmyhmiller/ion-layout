/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "compat.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern void clip_and_install(edge_t *, node_t *, pointf *, size_t,
                             splineInfo *);
extern port resolvePort(node_t *, node_t *, port *);
extern void do_graph_label(graph_t *);
extern void free_label(textlabel_t *);
extern void bezier_clip(inside_t *, bool (*)(inside_t *, pointf), pointf *,
                        bool);
extern boxf arrow_bb(pointf, pointf, double);
extern void arrow_flags(edge_t *, uint32_t *, uint32_t *);

/* The record is private to this engine; no ND_id/ND_order shared state is
 * repurposed, and cgraph owns its lifetime. */
typedef struct IonNodeIndex {
  Agrec_t header;
  size_t index;
} IonNodeIndex;
static size_t index_of(node_t *n) {
  return ((IonNodeIndex *)aggetrec(n, "IonNodeIndex", false))->index;
}
static double number(void *obj, const char *name, double dflt, double min) {
  char *s = agget(obj, (char *)name);
  if (!s || !*s)
    return dflt;
  char *end;
  double v = strtod(s, &end);
  return end == s || !isfinite(v) ? dflt : fmax(min, v);
}
/* Graphviz 13 permits up to four arrow shapes in 8-bit flag slots. Each
 * standard shape is at most 12 points long at unit arrowsize. Reserving
 * that bound keeps arbitrary compositions away from the opposite node. */
static unsigned arrow_count(uint32_t flags) {
  unsigned count = 0;
  while (flags) {
    if (flags & 0xff)
      count++;
    flags >>= 8;
  }
  return count;
}
static double end_clearance(edge_t *e, bool tail) {
  uint32_t s = 0, h = 0;
  arrow_flags(e, &s, &h);
  return 12.0 * arrow_count(tail ? s : h) * number(e, "arrowsize", 1, 0);
}
/* Reserve arrow silhouettes plus a visible gap, rather than just distinct
 * coordinates. Explicit ports remain under the DOT author's control. */
static double incoming_spacing(graph_t *g, node_t *n, size_t *count) {
  double spacing = 8.0;
  *count = 0;
  for (edge_t *e = agfstin(g, n); e; e = agnxtin(g, e)) {
    if (ED_head_port(e).defined)
      continue;
    (*count)++;
    uint32_t tail = 0, head = 0;
    arrow_flags(e, &tail, &head);
    double width = head ? 12.0 * number(e, "arrowsize", 1, 0) : 0;
    spacing = fmax(spacing, width + number(e, "penwidth", 1, 0) + 6.0);
  }
  return spacing;
}
void ion_reserve_incoming(IonGraph *ctx, double *widths, double *heights) {
  for (size_t i = 0; i < ctx->node_count; i++) {
    size_t count = 0;
    double spacing = incoming_spacing(ctx->graph, ctx->nodes[i], &count);
    if (count > 1) {
      double span = (count - 1) * spacing + spacing;
      widths[i] = fmax(widths[i], span);
      heights[i] = fmax(heights[i], span);
    }
  }
}
double ion_arrow_clearance(IonGraph *ctx) {
  double clearance = 0;
  for (size_t i = 0; i < ctx->edge_count; i++) {
    edge_t *e = ctx->edges[i];
    double length = end_clearance(e, true) + end_clearance(e, false);
    if (length > 0)
      clearance = fmax(clearance, length + 4.0);
  }
  return clearance;
}
static boxf arrow_box(pointf tip, pointf base, double scale) {
  // The native arrow bbox assumes one 10-point shape; a composed arrow's
  // clipped curve endpoint provides a conservative bound on its full length.
  return arrow_bb(tip, base,
                  fmax(scale, hypot(base.x - tip.x, base.y - tip.y) / 10.0));
}

static unsigned rank_kind(const char *rank) {
  if (!rank)
    return 0;
  if (!strcmp(rank, "min"))
    return 1;
  if (!strcmp(rank, "source"))
    return 2;
  if (!strcmp(rank, "max"))
    return 3;
  if (!strcmp(rank, "sink"))
    return 4;
  return 0;
}
static int walk(IonGraph *ctx, graph_t *g, size_t parent) {
  for (graph_t *sg = agfstsubg(g); sg; sg = agnxtsubg(sg)) {
    size_t group = parent;
    char *mode = agget(ctx->graph, "clusterrank");
    bool enable_clusters = !mode || !*mode || !strcmp(mode, "local");
    char *explicit_cluster = agget(sg, "cluster");
    bool is_cluster =
        enable_clusters &&
        (!strncasecmp(agnameof(sg), "cluster", 7) ||
         (explicit_cluster && (!strcmp(explicit_cluster, "true") ||
                               !strcmp(explicit_cluster, "1"))));
    if (is_cluster) {
      size_t count = ctx->cluster_count + 1;
      IonCluster *clusters = realloc(ctx->clusters, count * sizeof(*clusters));
      if (!clusters)
        return -1;
      ctx->clusters = clusters;
      graph_t **graphs = realloc(ctx->cluster_graphs, count * sizeof(*graphs));
      if (!graphs)
        return -1;
      ctx->cluster_graphs = graphs;
      group = ctx->cluster_count++;
      agbindrec(sg, "Agraphinfo_t", sizeof(Agraphinfo_t), true);
      GD_parent(sg) =
          parent == SIZE_MAX ? ctx->graph : ctx->cluster_graphs[parent];
      GD_gvc(sg) = GD_gvc(ctx->graph);
      GD_charset(sg) = GD_charset(ctx->graph);
      do_graph_label(sg);
      ctx->clusters[group] =
          (IonCluster){.parent = parent,
                       .margin = number(sg, "margin", 8.0, 0.0),
                       .label_w = GD_label(sg) ? GD_label(sg)->dimen.x : 0.0,
                       .label_h = GD_label(sg) ? GD_label(sg)->dimen.y : 0.0};
      ctx->cluster_graphs[group] = sg;
      graph_t *pg = GD_parent(sg);
      size_t n = GD_n_cluster(pg) + 1;
      graph_t **children = realloc(GD_clust(pg), (n + 1) * sizeof(*children));
      if (!children)
        return -1;
      GD_clust(pg) = children;
      GD_clust(pg)[n] = sg;
      GD_n_cluster(pg) = (int)n;
      for (node_t *node = agfstnode(sg); node; node = agnxtnode(sg, node))
        ctx->node_constraints[index_of(node)].cluster = group;
    }
    char *rank = agget(sg, "rank");
    if (rank && (!strcmp(rank, "same") || rank_kind(rank))) {
      node_t *first = agfstnode(sg);
      if (first) {
        size_t representative = index_of(first);
        for (node_t *n = first; n; n = agnxtnode(sg, n)) {
          size_t i = index_of(n), old = ctx->node_constraints[i].group;
          if (old != SIZE_MAX && old != representative)
            for (size_t j = 0; j < ctx->node_count; j++)
              if (ctx->node_constraints[j].group == old)
                ctx->node_constraints[j].group = representative;
          ctx->node_constraints[i].group = representative;
          if (rank_kind(rank))
            ctx->node_constraints[i].kind = rank_kind(rank);
        }
      }
    }
    if (walk(ctx, sg, group))
      return -1;
  }
  return 0;
}
int ion_extract_constraints(IonGraph *ctx) {
  ctx->node_constraints = calloc(ctx->node_count ? ctx->node_count : 1,
                                 sizeof(*ctx->node_constraints));
  ctx->edge_constraints = calloc(ctx->edge_count ? ctx->edge_count : 1,
                                 sizeof(*ctx->edge_constraints));
  if (!ctx->node_constraints || !ctx->edge_constraints)
    return -1;
  for (size_t i = 0; i < ctx->node_count; i++)
    ctx->node_constraints[i] =
        (IonNodeConstraint){.group = SIZE_MAX, .cluster = SIZE_MAX};
  for (size_t i = 0; i < ctx->edge_count; i++) {
    edge_t *e = ctx->edges[i];
    char *constraint = agget(e, "constraint");
    ctx->edge_constraints[i] = (IonEdgeConstraint){
        .minlen = (unsigned)fmin(65535.0, number(e, "minlen", 1, 0)),
        .constraint = !(constraint && (!strcmp(constraint, "false") ||
                                       !strcmp(constraint, "0"))),
        .weight = number(e, "weight", 1, 0)};
  }
  return walk(ctx, ctx->graph, SIZE_MAX);
}
void ion_install_clusters(IonGraph *ctx, double height) {
  for (size_t i = 0; i < ctx->cluster_count; i++) {
    IonRect r = ctx->clusters[i].bounds;
    graph_t *g = ctx->cluster_graphs[i];
    GD_bb(g) = (boxf){.LL = {r.x0, height - r.y1}, .UR = {r.x1, height - r.y0}};
    if (GD_label(g)) {
      textlabel_t *l = GD_label(g);
      unsigned pos = GD_label_pos(g);
      double margin = ctx->clusters[i].margin;
      l->pos.x = (r.x0 + r.x1) / 2.0;
      if (pos & 2)
        l->pos.x = r.x0 + margin + l->dimen.x / 2.0;
      if (pos & 4)
        l->pos.x = r.x1 - margin - l->dimen.x / 2.0;
      l->pos.y = pos & 1 ? GD_bb(g).UR.y - margin - l->dimen.y / 2.0
                         : GD_bb(g).LL.y + margin + l->dimen.y / 2.0;
      l->set = true;
    }
  }
}
void ion_free_constraints(IonGraph *ctx) {
  free(ctx->node_constraints);
  free(ctx->edge_constraints);
  free(ctx->clusters);
  free(ctx->cluster_graphs);
}
void ion_cleanup_clusters(graph_t *g) {
  for (int i = 1; i <= GD_n_cluster(g); i++) {
    graph_t *c = GD_clust(g)[i];
    ion_cleanup_clusters(c);
    free_label(GD_label(c));
    GD_label(c) = NULL;
  }
  free(GD_clust(g));
  GD_clust(g) = NULL;
  GD_n_cluster(g) = 0;
}
static bool segment_hits(pointf, pointf, boxf);

static bool never_merge(node_t *n) {
  (void)n;
  return false;
}
static pointf lerp(pointf a, pointf b, double t) {
  return (pointf){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}
static pointf flip(IonPoint p, double h) { return (pointf){p.x, h - p.y}; }

/* Aiming point inside the shape/port and an escape point beyond its node
 * rectangle. The native clipper trims the attachment segment correctly. */
typedef struct Endpoint {
  pointf aim, escape;
} Endpoint;
static Endpoint endpoint(node_t *n, node_t *other, port *p, pointf preferred,
                         pointf outward, double offset, double lead) {
  pointf c = ND_coord(n);
  Endpoint end;
  if (p->dyna)
    *p = resolvePort(n, other, p);
  if (p->defined) {
    end.aim = (pointf){c.x + p->p.x, c.y + p->p.y};
    if (p->constrained) {
      outward = (pointf){cos(p->theta), sin(p->theta)};
    } else {
      pointf target = ND_coord(other);
      double dx = target.x - end.aim.x, dy = target.y - end.aim.y;
      if (fabs(dx) > fabs(dy))
        outward = (pointf){dx >= 0 ? 1 : -1, 0};
      else
        outward = (pointf){0, dy >= 0 ? 1 : -1};
    }
  } else {
    end.aim = preferred;
    if (fabs(outward.x) > fabs(outward.y)) {
      end.aim.x = c.x;
      end.aim.y = fmax(c.y - ND_ht(n) / 2.0 + 2,
                       fmin(c.y + ND_ht(n) / 2.0 - 2, preferred.y + offset));
    } else {
      end.aim.y = c.y;
      end.aim.x = fmax(c.x - ND_lw(n) + 2,
                       fmin(c.x + ND_rw(n) - 2, preferred.x + offset));
    }
    if (!p->clip)
      end.aim = c;
  }
  // Compass directions can be diagonal. Preserve their true tangent as
  // far as the escape point; the grid route starts outside the node.
  double tx = INFINITY, ty = INFINITY;
  if (outward.x > 1e-8)
    tx = (c.x + ND_rw(n) - end.aim.x) / outward.x;
  if (outward.x < -1e-8)
    tx = (c.x - ND_lw(n) - end.aim.x) / outward.x;
  if (outward.y > 1e-8)
    ty = (c.y + ND_ht(n) / 2 - end.aim.y) / outward.y;
  if (outward.y < -1e-8)
    ty = (c.y - ND_ht(n) / 2 - end.aim.y) / outward.y;
  double t = fmax(0, fmin(tx, ty)) + lead;
  end.escape = (pointf){end.aim.x + t * outward.x, end.aim.y + t * outward.y};
  return end;
}
static pointf unit(pointf a, pointf b) {
  double x = b.x - a.x, y = b.y - a.y, len = hypot(x, y);
  return len > 1e-8 ? (pointf){x / len, y / len} : (pointf){0, -1};
}
static bool in_cluster(inside_t *ctx, pointf p) {
  boxf b = *ctx->s.bp;
  return p.x >= b.LL.x && p.x <= b.UR.x && p.y >= b.LL.y && p.y <= b.UR.y;
}
static bool trim_cluster(IonGraph *ctx, edge_t *e, pointf *points,
                         size_t *count, bool tail) {
  char *compound = agget(ctx->graph, "compound");
  if (!compound || (strcmp(compound, "true") && strcmp(compound, "1")))
    return false;
  char *name = agget(e, tail ? "ltail" : "lhead");
  if (!name || !*name)
    return false;
  graph_t *cluster = NULL;
  for (size_t i = 0; i < ctx->cluster_count; i++)
    if (!strcmp(name, agnameof(ctx->cluster_graphs[i])))
      cluster = ctx->cluster_graphs[i];
  if (!cluster || !agcontains(cluster, tail ? agtail(e) : aghead(e))) {
    agwarningf("ion: %s does not contain the %s endpoint\n", name,
               tail ? "tail" : "head");
    return false;
  }
  inside_t inside = {.s = {.bp = &GD_bb(cluster)}};
  if (tail) {
    for (size_t i = 0; i + 3 < *count; i += 3)
      if (!in_cluster(&inside, points[i + 3])) {
        bezier_clip(&inside, in_cluster, points + i, true);
        memmove(points, points + i, (*count - i) * sizeof(*points));
        *count -= i;
        return true;
      }
  } else {
    for (size_t end = *count - 1; end >= 3; end -= 3)
      if (!in_cluster(&inside, points[end - 3])) {
        bezier_clip(&inside, in_cluster, points + end - 3, false);
        *count = end + 1;
        return true;
      }
  }
  agwarningf("ion: compound edge remains inside cluster %s\n", name);
  return false;
}

/* Explicit constraints can move entire ranks and clusters. Feedback edges
 * still get Ion's outside channel, even when their old geometry is invalid. */
static int route_channel(IonPoint start, IonPoint end, const IonRect *boxes,
                         size_t n, bool horizontal, unsigned channel,
                         double lane, double preferred_cross, const IonPoint *aims, IonPoint **path, size_t *len) {
  double cross = horizontal ? fmin(start.y, end.y) : fmax(start.x, end.x);
  for (size_t i = 0; i < n; i++)
    cross = horizontal ? fmin(cross, boxes[i].y0) : fmax(cross, boxes[i].x1);
  cross += (horizontal ? -1 : 1) * (16.0 + channel * 12.0);
  if (isfinite(preferred_cross))
    cross = horizontal ? fmin(cross, preferred_cross) : fmax(cross, preferred_cross);
  IonPoint waypoints[4] = {
      start,
      horizontal ? (IonPoint){start.x, cross} : (IonPoint){cross, start.y},
      horizontal ? (IonPoint){end.x, cross} : (IonPoint){cross, end.y}, end};
  return ion_route_attached_compute(waypoints, 3, end, aims, boxes, n, lane, path, len);
}

static int route_attached(pointf start, pointf end, pointf tail_aim,
                          pointf head_aim, const IonRect *boxes, size_t n,
                          double lane, IonPoint **path, size_t *len) {
  IonPoint via = {start.x, start.y};
  IonPoint aims[2] = {{tail_aim.x, tail_aim.y}, {head_aim.x, head_aim.y}};
  return ion_route_attached_compute(&via, 1, (IonPoint){end.x, end.y}, aims,
                                    boxes, n, lane, path, len);
}

static bool curve_hits(pointf *p, boxf r, unsigned depth);

int ion_route_edge(IonGraph *ctx, size_t ei, IonRoute preferred,
                   const IonPoint *raw, double height) {
  edge_t *e = ctx->edges[ei];
  node_t *tail = agtail(e), *head = aghead(e);
  pointf t = ND_coord(tail), h = ND_coord(head);
  pointf start = t, tip = h, out = {0, -1}, in = {0, 1};
  bool routed = preferred.point_count >= 4;
  /* Compiler block order can put unrelated blocks across an Ion channel.
   * Certify the preferred trunk before retaining any part of it. */
  for (size_t ni = 0; routed && ni < ctx->node_count; ni++) {
    node_t *n = ctx->nodes[ni];
    if (n == tail || n == head)
      continue;
    pointf c = ND_coord(n);
    boxf box = {{c.x - ND_lw(n) + 0.001, c.y - ND_ht(n) / 2 + 0.001},
                {c.x + ND_rw(n) - 0.001, c.y + ND_ht(n) / 2 - 0.001}};
    for (size_t pi = 0; pi + 3 < preferred.point_count; pi += 3) {
      pointf curve[4];
      for (size_t k = 0; k < 4; k++)
        curve[k] = flip(raw[preferred.offset + pi + k], height);
      if (curve_hits(curve, box, 0)) {
        routed = false;
        break;
      }
    }
  }
  char *rankdir = agget(ctx->graph, "rankdir");
  bool horizontal =
      rankdir && (!strcmp(rankdir, "LR") || !strcmp(rankdir, "RL"));
  bool feedback = ctx->edge_constraints[ei].kind == 1;
  double preferred_cross = NAN;
  if (routed && feedback) {
    preferred_cross = horizontal ? INFINITY : -INFINITY;
    for (size_t i = 0; i < preferred.point_count; i++) {
      pointf p = flip(raw[preferred.offset + i], height);
      preferred_cross = horizontal ? fmin(preferred_cross, p.y)
                                   : fmax(preferred_cross, p.x);
    }
  }
  if (routed) {
    start = flip(raw[preferred.offset], height);
    tip = flip(preferred.arrow_tip, height);
    out = unit(start, flip(raw[preferred.offset + 1], height));
    in = unit(tip,
              flip(raw[preferred.offset + preferred.point_count - 1], height));
  } else if (fabs(t.x - h.x) > fabs(t.y - h.y)) {
    out = (pointf){h.x >= t.x ? 1 : -1, 0};
    in = (pointf){-out.x, 0};
  } else {
    out = (pointf){0, h.y >= t.y ? 1 : -1};
    in = (pointf){0, -out.y};
  }
  if (feedback && !routed) {
    bool reverse =
        rankdir && (!strcmp(rankdir, "BT") || !strcmp(rankdir, "RL"));
    out = horizontal ? (pointf){reverse ? -1 : 1, 0}
                     : (pointf){0, reverse ? 1 : -1};
    in = horizontal ? (pointf){0, -1} : (pointf){1, 0};
  }
  size_t multiplicity = 0, ordinal = 0;
  for (edge_t *other = agfstout(ctx->graph, tail); other;
       other = agnxtout(ctx->graph, other))
    if (aghead(other) == head) {
      if (other == e)
        ordinal = multiplicity;
      multiplicity++;
    }
  size_t incoming = 0, input_ordinal = 0;
  for (edge_t *other = agfstin(ctx->graph, head); other;
       other = agnxtin(ctx->graph, other)) {
    if (ED_head_port(other).defined)
      continue;
    if (AGID(other) == AGID(e))
      input_ordinal = incoming;
    incoming++;
  }
  bool separate_head = incoming > 1 && !ED_head_port(e).defined;
  if (separate_head) {
    double span =
        fabs(in.x) > fabs(in.y) ? ND_ht(head) : ND_lw(head) + ND_rw(head);
    size_t auto_count = 0;
    double spacing = incoming_spacing(ctx->graph, head, &auto_count);
    double step = fmin(spacing, fmax(0.0, span - spacing) / (incoming - 1));
    double displacement =
        ((double)input_ordinal - ((double)incoming - 1) / 2.0) * step;
    if (fabs(in.x) > fabs(in.y))
      tip.y = h.y + displacement;
    else
      tip.x = h.x + displacement;
  }
  double spread =
      multiplicity > 1
          ? ((double)ordinal - ((double)multiplicity - 1) / 2.0) * 6.0
          : 0;
  bool special = !routed || ED_tail_port(e).defined ||
                 ED_head_port(e).defined || multiplicity > 1 || separate_head ||
                 !ED_tail_port(e).clip || !ED_head_port(e).clip;
  double lead =
      fmax(8.0, fmax(end_clearance(e, true), end_clearance(e, false)) + 20.0) +
      ordinal * 4.0;
  double head_lead = lead + (separate_head ? input_ordinal * 12.0 : 0);
  Endpoint a = endpoint(tail, head, &ED_tail_port(e), start, out, spread, lead);
  Endpoint b = endpoint(head, tail, &ED_head_port(e), tip, in,
                        separate_head ? 0 : spread, head_lead);
  pointf *points = NULL;
  size_t count = 0;
  if (!special) {
    count = preferred.point_count;
    points = malloc(count * sizeof(*points));
    if (!points)
      return -1;
    for (size_t i = 0; i < count; i++)
      points[i] = flip(raw[preferred.offset + i], height);
    points[0] = a.aim;
    points[count - 1] = b.aim;
    points[1] = lerp(points[0], points[3], 1.0 / 3);
    points[2] = lerp(points[0], points[3], 2.0 / 3);
    points[count - 3] = lerp(points[count - 4], points[count - 1], 1.0 / 3);
    points[count - 2] = lerp(points[count - 4], points[count - 1], 2.0 / 3);
  } else {
    size_t box_count = ctx->node_count + ctx->cluster_count;
    IonRect *boxes = calloc(box_count ? box_count : 1, sizeof(*boxes));
    if (!boxes)
      return -1;
    for (size_t i = 0; i < ctx->node_count; i++) {
      node_t *n = ctx->nodes[i];
      pointf c = ND_coord(n);
      boxes[i] = (IonRect){c.x - ND_lw(n), c.y - ND_ht(n) / 2, c.x + ND_rw(n),
                           c.y + ND_ht(n) / 2};
    }
    size_t actual = ctx->node_count;
    for (size_t i = 0; i < ctx->cluster_count; i++) {
      graph_t *g = ctx->cluster_graphs[i];
      textlabel_t *l = GD_label(g);
      if (l)
        boxes[actual++] = (IonRect){
            l->pos.x - l->dimen.x / 2 - 2, l->pos.y - l->dimen.y / 2 - 2,
            l->pos.x + l->dimen.x / 2 + 2, l->pos.y + l->dimen.y / 2 + 2};
    }
    /* Keep Ion's existing loop/channel trunk when only the automatic
     * input slot changed. Rebuild the final approach against obstacles
     * rather than replacing the entire loop route with a shortest path. */
    if (routed && separate_head && multiplicity == 1 &&
        preferred.point_count > 4 && !ED_tail_port(e).defined &&
        !ED_head_port(e).defined && ED_tail_port(e).clip &&
        ED_head_port(e).clip) {
      size_t prefix = preferred.point_count - 3;
      pointf junction = flip(raw[preferred.offset + prefix - 1], height);
      IonPoint *suffix = NULL;
      size_t len = 0;
      int status = route_attached(junction, b.escape, junction, b.aim,
                                  boxes, actual, 4.0, &suffix, &len);
      /* A retained rounded corner is only valid when the new suffix
       * continues its tangent. Otherwise the splice makes a hook or a
       * cusp: rebuild the channel instead of doubling back. */
      pointf tangent = unit(flip(raw[preferred.offset + prefix - 2], height),
                            junction);
      pointf continuation = len > 1
                                ? unit(junction, (pointf){suffix[1].x,
                                                          suffix[1].y})
                                : (pointf){0, 0};
      bool smooth = tangent.x * continuation.x +
                        tangent.y * continuation.y > 1.0 - 1e-6;
      if (!status && len && smooth) {
        count = prefix + len - 1;
        points = malloc(count * sizeof(*points));
        if (!points) {
          free(boxes);
          ion_layout_free_points(suffix, len);
          return -1;
        }
        for (size_t i = 0; i < prefix; i++)
          points[i] = flip(raw[preferred.offset + i], height);
        points[0] = a.aim;
        points[1] = lerp(points[0], points[3], 1.0 / 3);
        points[2] = lerp(points[0], points[3], 2.0 / 3);
        for (size_t i = 1; i < len; i++)
          points[prefix + i - 1] = (pointf){suffix[i].x, suffix[i].y};
      }
      ion_layout_free_points(suffix, len);
    }
    if (!points) {
      IonPoint *path = NULL;
      size_t len = 0;
      int status = -1;
      pointf dirs[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
      // Automatic attachments may approach a node on a different side
      // when a cluster label blocks the preferred one. Explicit compass
      // constraints remain fixed. Both attachment segments and the middle
      // route must be clear, including cluster label obstacles.
      for (int ta = 0; ta < 5 && status; ta++)
        for (int hb = 0; hb < 5 && status; hb++) {
          if (ta && ED_tail_port(e).defined)
            continue;
          if (hb && ED_head_port(e).defined)
            continue;
          a = endpoint(tail, head, &ED_tail_port(e), start,
                       ta ? dirs[ta - 1] : out, spread, lead);
          b = endpoint(head, tail, &ED_head_port(e), tip,
                       hb ? dirs[hb - 1] : in, separate_head ? 0 : spread,
                       head_lead);
          bool attachments_clear = true;
          for (size_t j = 0; j < actual; j++) {
            boxf obstacle = {.LL = {boxes[j].x0 + 0.001, boxes[j].y0 + 0.001},
                             .UR = {boxes[j].x1 - 0.001, boxes[j].y1 - 0.001}};
            if ((j != index_of(tail) &&
                 segment_hits(a.aim, a.escape, obstacle)) ||
                (j != index_of(head) &&
                 segment_hits(b.aim, b.escape, obstacle))) {
              attachments_clear = false;
              break;
            }
          }
          if (!attachments_clear)
            continue;
          IonPoint aims[2] = {{a.aim.x, a.aim.y}, {b.aim.x, b.aim.y}};
          if (feedback)
            status =
                route_channel((IonPoint){a.escape.x, a.escape.y},
                              (IonPoint){b.escape.x, b.escape.y}, boxes, actual,
                              horizontal, ctx->edge_constraints[ei].channel,
                              4.0 + ordinal * 4.0, preferred_cross, aims, &path, &len);
          else
            status =
                route_attached(a.escape, b.escape, a.aim, b.aim, boxes,
                               actual, 4.0 + ordinal * 4.0, &path, &len);
        }
      free(boxes);
      if (status || len == 0) {
        ion_layout_free_points(path, len);
        agerrorf("ion: no obstacle-free route for %s -> %s\n", agnameof(tail),
                 agnameof(head));
        return -1;
      }
      count = len;
      points = malloc(count * sizeof(*points));
      if (!points) {
        ion_layout_free_points(path, len);
        return -1;
      }
      for (size_t i = 0; i < len; i++)
        points[i] = (pointf){path[i].x, path[i].y};
      ion_layout_free_points(path, len);
    } else {
      free(boxes);
    }
  }
  bool tail_clip = ED_tail_port(e).clip, head_clip = ED_head_port(e).clip;
  if (trim_cluster(ctx, e, points, &count, true))
    ED_tail_port(e).clip = false;
  if (trim_cluster(ctx, e, points, &count, false))
    ED_head_port(e).clip = false;
  splineInfo info = {.splineMerge = never_merge, .ignoreSwap = true};
  clip_and_install(e, head, points, count, &info);
  if (ED_spl(e))
    for (size_t i = 0; i < ED_spl(e)->size; i++) {
      bezier *b = &ED_spl(e)->list[i];
      boxf bb = GD_bb(ctx->graph);
      double size = number(e, "arrowsize", 1, 0);
      boxf arrows[2];
      size_t n = 0;
      if (b->sflag)
        arrows[n++] = arrow_box(b->sp, b->list[0], size);
      if (b->eflag)
        arrows[n++] = arrow_box(b->ep, b->list[b->size - 1], size);
      for (size_t j = 0; j < n; j++) {
        bb.LL.x = fmin(bb.LL.x, arrows[j].LL.x);
        bb.LL.y = fmin(bb.LL.y, arrows[j].LL.y);
        bb.UR.x = fmax(bb.UR.x, arrows[j].UR.x);
        bb.UR.y = fmax(bb.UR.y, arrows[j].UR.y);
      }
      GD_bb(ctx->graph) = bb;
    }
  ED_tail_port(e).clip = tail_clip;
  ED_head_port(e).clip = head_clip;
  free(points);
  return 0;
}

static bool overlaps(boxf a, boxf b) {
  return a.LL.x < b.UR.x && a.UR.x > b.LL.x && a.LL.y < b.UR.y &&
         a.UR.y > b.LL.y;
}
static boxf label_box(textlabel_t *l, pointf p, double pad) {
  return (boxf){.LL = {p.x - l->dimen.x / 2 - pad, p.y - l->dimen.y / 2 - pad},
                .UR = {p.x + l->dimen.x / 2 + pad, p.y + l->dimen.y / 2 + pad}};
}
static pointf cubic(pointf *p, double t) {
  pointf a = lerp(p[0], p[1], t), b = lerp(p[1], p[2], t),
         c = lerp(p[2], p[3], t);
  return lerp(lerp(a, b, t), lerp(b, c, t), t);
}
static bool segment_hits(pointf a, pointf b, boxf r) {
  double lo = 0, hi = 1, dx = b.x - a.x, dy = b.y - a.y;
  double p[4] = {-dx, dx, -dy, dy},
         q[4] = {a.x - r.LL.x, r.UR.x - a.x, a.y - r.LL.y, r.UR.y - a.y};
  for (int i = 0; i < 4; i++) {
    if (fabs(p[i]) < 1e-9) {
      if (q[i] < 0)
        return false;
    } else {
      double t = q[i] / p[i];
      if (p[i] < 0)
        lo = fmax(lo, t);
      else
        hi = fmin(hi, t);
      if (lo > hi)
        return false;
    }
  }
  return true;
}
/* Recursive convex-hull subdivision is conservative: labels can never
 * slip between fixed curve samples. Bounding hulls smaller than a quarter
 * point are still treated as occupied. */
static bool curve_hits(pointf *p, boxf r, unsigned depth) {
  boxf hull = {.LL = p[0], .UR = p[0]};
  for (int i = 1; i < 4; i++) {
    hull.LL.x = fmin(hull.LL.x, p[i].x);
    hull.LL.y = fmin(hull.LL.y, p[i].y);
    hull.UR.x = fmax(hull.UR.x, p[i].x);
    hull.UR.y = fmax(hull.UR.y, p[i].y);
  }
  if (hull.UR.x < r.LL.x || hull.LL.x > r.UR.x || hull.UR.y < r.LL.y ||
      hull.LL.y > r.UR.y)
    return false;
  if (depth == 20 || fmax(hull.UR.x - hull.LL.x, hull.UR.y - hull.LL.y) < .25)
    return true;
  pointf a = lerp(p[0], p[1], .5), b = lerp(p[1], p[2], .5),
         c = lerp(p[2], p[3], .5);
  pointf d = lerp(a, b, .5), f = lerp(b, c, .5), m = lerp(d, f, .5);
  pointf left[4] = {p[0], a, d, m}, right[4] = {m, f, c, p[3]};
  return curve_hits(left, r, depth + 1) || curve_hits(right, r, depth + 1);
}
static bool label_clear(IonGraph *ctx, boxf r, boxf *placed, size_t used) {
  for (size_t i = 0; i < ctx->node_count; i++)
    if (overlaps(r, ND_bb(ctx->nodes[i])))
      return false;
  for (size_t i = 0; i < used; i++)
    if (overlaps(r, placed[i]))
      return false;
  for (size_t i = 0; i < ctx->edge_count; i++) {
    edge_t *e = ctx->edges[i];
    splines *s = ED_spl(e);
    if (!s)
      continue;
    for (size_t k = 0; k < s->size; k++) {
      bezier *b = &s->list[k];
      for (size_t j = 0; j + 3 < b->size; j += 3)
        if (curve_hits(b->list + j, r, 0))
          return false;
      double size = number(e, "arrowsize", 1, 0);
      if (b->sflag && overlaps(r, arrow_box(b->sp, b->list[0], size)))
        return false;
      if (b->eflag && overlaps(r, arrow_box(b->ep, b->list[b->size - 1], size)))
        return false;
    }
  }
  return true;
}
static void grow(boxf *bb, boxf r) {
  bb->LL.x = fmin(bb->LL.x, r.LL.x);
  bb->LL.y = fmin(bb->LL.y, r.LL.y);
  bb->UR.x = fmax(bb->UR.x, r.UR.x);
  bb->UR.y = fmax(bb->UR.y, r.UR.y);
}
void ion_place_labels(IonGraph *ctx) {
  size_t capacity = ctx->edge_count * 4 + ctx->cluster_count;
  boxf *placed = calloc(capacity ? capacity : 1, sizeof(*placed));
  if (!placed) {
    agerrorf("ion: unable to allocate label obstacles\n");
    return;
  }
  size_t used = 0;
  for (size_t i = 0; i < ctx->cluster_count; i++) {
    textlabel_t *l = GD_label(ctx->cluster_graphs[i]);
    if (l)
      placed[used++] = label_box(l, l->pos, 2);
  }
  for (size_t i = 0; i < ctx->edge_count; i++) {
    edge_t *e = ctx->edges[i];
    splines *s = ED_spl(e);
    if (!s || !s->size)
      continue;
    bezier *b = &s->list[0];
    textlabel_t *labels[4] = {ED_label(e), ED_head_label(e), ED_tail_label(e),
                              ED_xlabel(e)};
    for (int kind = 0; kind < 4; kind++) {
      textlabel_t *l = labels[kind];
      if (!l)
        continue;
      pointf anchor = kind == 1 ? (b->eflag ? b->ep : b->list[b->size - 1])
                      : kind == 2
                          ? (b->sflag ? b->sp : b->list[0])
                          : cubic(b->list + 3 * ((b->size - 1) / 6), 0.5);
      bool found = false;
      pointf pos = anchor;
      // Search around the anchor with deterministic nearest rings. The
      // unbounded exterior guarantees a free box without clipping it.
      for (size_t ring = 0; !found; ring++)
        for (int side = 0; side < 8; side++) {
          double dx = (l->dimen.x / 2 + 5 + ring * 12),
                 dy = (l->dimen.y / 2 + 5 + ring * 12);
          static const int x[8] = {1, -1, 0, 0, 1, -1, 1, -1},
                           y[8] = {0, 0, 1, -1, 1, 1, -1, -1};
          pos = (pointf){anchor.x + x[side] * dx, anchor.y + y[side] * dy};
          boxf r = label_box(l, pos, 2);
          if (label_clear(ctx, r, placed, used)) {
            placed[used++] = r;
            l->pos = pos;
            l->set = true;
            grow(&GD_bb(ctx->graph), r);
            found = true;
            break;
          }
        }
    }
  }
  free(placed);
}
