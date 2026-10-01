/* Exercise the public libgvc layout/free-layout lifecycle, including reuse
 * of the same graph and the same context across different graph shapes. */
#include <graphviz/cgraph.h>
#include <graphviz/gvc.h>
#include <graphviz/gvplugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern gvplugin_library_t gvplugin_ion_LTX_library;

static int messages;
static int report(char *message) {
  fputs(message, stderr);
  messages++;
  return 0;
}

int main(void) {
  agseterrf(report);
  const char *sources[] = {
      "digraph {label=Title;a->b[label=\"Edge\"];a->c;c->b;}",
      "digraph {rankdir=LR;compound=true;subgraph cluster_a "
      "{label=A;a;}subgraph cluster_b "
      "{label=B;b;}a->b[lhead=cluster_b,ltail=cluster_a];}",
      "digraph "
      "{node[shape=record];{rank=same;a;b;}a[label=\"<p>Left|<q>Right\"];a:q:e-"
      ">b:w;a->b[dir=both,arrowsize=3];}",
      "digraph {subgraph cluster_empty {label=Empty;}}",
      "digraph {}",
  };
  GVC_t *ctx = gvContext();
  if (!ctx)
    return 1;
  const char *engine = getenv("LIFECYCLE_ENGINE");
  if (!engine)
    engine = "ion";
  if (!strcmp(engine, "ion"))
    gvAddLibrary(ctx, &gvplugin_ion_LTX_library);
  for (size_t s = 0; s < sizeof(sources) / sizeof(sources[0]); s++) {
    Agraph_t *g = agmemread(sources[s]);
    if (!g)
      return 1;
    char *reference = NULL;
    size_t reference_len = 0;
    for (int repeat = 0; repeat < 20; repeat++) {
      char *data = NULL;
      size_t length = 0;
      if (gvLayout(ctx, g, engine) ||
          gvRenderData(ctx, g, "svg", &data, &length))
        return 1;
      if (!repeat) {
        reference = malloc(length);
        if (!reference)
          return 1;
        memcpy(reference, data, length);
        reference_len = length;
      } else if (reference_len != length || memcmp(reference, data, length)) {
        fprintf(
            stderr,
            "non-deterministic render after graph reuse: case %zu repeat %d\n",
            s, repeat);
        return 1;
      }
      gvFreeRenderData(data);
      gvFreeLayout(ctx, g);
    }
    free(reference);
    agclose(g);
  }
  gvFreeContext(ctx);
  if (messages)
    return 1;
  puts("100 repeated libgvc layouts passed");
  return 0;
}
