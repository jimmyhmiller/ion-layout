/* Graphviz semantic adapter: rank sets, clusters, ports and label obstacles. */
#pragma once
#include "ion_layout.h"
#include <graphviz/types.h>

typedef struct IonGraph {
  graph_t *graph;
  size_t node_count, edge_count, cluster_count;
  node_t **nodes;
  edge_t **edges;
  IonNodeConstraint *node_constraints;
  IonEdgeConstraint *edge_constraints;
  IonCluster *clusters;
  graph_t **cluster_graphs;
} IonGraph;

int ion_extract_constraints(IonGraph *ctx);
double ion_arrow_clearance(IonGraph *ctx);
void ion_reserve_incoming(IonGraph *ctx, double *widths, double *heights);
void ion_install_clusters(IonGraph *ctx, double height);
void ion_free_constraints(IonGraph *ctx);
void ion_cleanup_clusters(graph_t *g);
int ion_route_edge(IonGraph *ctx, size_t edge_index, IonRoute preferred,
                   const IonPoint *preferred_points, double height);
void ion_place_labels(IonGraph *ctx);
