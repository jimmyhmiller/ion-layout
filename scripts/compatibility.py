#!/usr/bin/env python3
"""Semantic integration tests against the rendered Graphviz JSON/SVG output."""
import json
import math
import os
import random
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parent.parent
ENV = {**os.environ, 'GVBINDIR': str(ROOT / 'target/graphviz')}
ARTIFACTS = ROOT / 'target/compatibility'
ARTIFACTS.mkdir(parents=True, exist_ok=True)


def render(source, name='probe', fmt='json'):
    result = subprocess.run(['dot', '-Kion', '-T' + fmt], input=source.encode(),
                            capture_output=True, env=ENV, timeout=30)
    if result.returncode or result.stderr:
        raise AssertionError(f'{name}: exit {result.returncode}: {result.stderr.decode()}')
    (ARTIFACTS / (name + '.dot')).write_text(source)
    (ARTIFACTS / (name + '.' + fmt)).write_bytes(result.stdout)
    return json.loads(result.stdout) if fmt == 'json' else result.stdout


def nodes(graph):
    return {obj['name']: obj for obj in graph.get('objects', []) if 'pos' in obj}


def point(value):
    return tuple(map(float, value.split(',')))


def box(node):
    x, y = point(node['pos'])
    w, h = float(node['width']) * 72, float(node['height']) * 72
    return x - w / 2, y - h / 2, x + w / 2, y + h / 2


def curves(edge):
    for op in edge.get('_draw_', []):
        if op['op'] in ('b', 'B'):
            yield op['points']


def ends(edge):
    pts = next(curves(edge))
    endpoints = [pts[0], pts[-1]]
    for value in edge['pos'].split():
        if value.startswith('s,'):
            endpoints[0] = point(value[2:])
        elif value.startswith('e,'):
            endpoints[1] = point(value[2:])
    return endpoints


def assert_geometry(test, graph):
    ns = nodes(graph)
    by_id = {n['_gvid']: n for n in ns.values()}
    bb = tuple(map(float, graph['bb'].split(',')))
    for n in ns.values():
        r = box(n)
        test.assertTrue(r[0] >= bb[0] - 1 and r[1] >= bb[1] - 1 and
                        r[2] <= bb[2] + 1 and r[3] <= bb[3] + 1, n['name'])
    for edge in graph.get('edges', []):
        test.assertTrue(list(curves(edge)), f'missing route {edge}')
        for pts in curves(edge):
            for i in range(0, len(pts) - 1, 3):
                for sample in range(101):
                    t = sample / 100
                    weights = [(1-t)**3, 3*(1-t)**2*t, 3*(1-t)*t*t, t**3]
                    x = sum(weights[j] * pts[i+j][0] for j in range(4))
                    y = sum(weights[j] * pts[i+j][1] for j in range(4))
                    test.assertTrue(bb[0]-1 <= x <= bb[2]+1 and bb[1]-1 <= y <= bb[3]+1)
                    for nid, n in by_id.items():
                        if nid in (edge['tail'], edge['head']):
                            continue
                        l, b, r, top = box(n)
                        test.assertFalse(l+1 < x < r-1 and b+1 < y < top-1,
                                         f"route through {n['name']}")


class Compatibility(unittest.TestCase):
    def test_feedback_approaches_do_not_hook(self):
        source = (ROOT / 'tests/compiler/v8-switch-build.dot').read_text()
        graph = render(source, 'v8-feedback-hooks')
        ns = nodes(graph)
        by_id = {n['_gvid']: n for n in ns.values()}
        for edge in graph['edges']:
            if by_id[edge['head']]['name'] != 'B9':
                continue
            self.assertIn(by_id[edge['tail']]['name'], ['B6', 'B7', 'B8'])
            pts = next(curves(edge))
            # These backedges rise to their attachment level and turn left.
            # A retained obsolete corner used to rise above it then turn down.
            self.assertLessEqual(max(p[1] for p in pts), pts[-1][1] + 0.05)
            for i in range(3, len(pts) - 1, 3):
                incoming = [pts[i][d] - pts[i-1][d] for d in range(2)]
                outgoing = [pts[i+1][d] - pts[i][d] for d in range(2)]
                self.assertGreaterEqual(sum(a*b for a, b in zip(incoming, outgoing)), -0.01)

    def test_shape_clipping(self):
        for direction in ('TB', 'LR', 'BT', 'RL'):
            g = render(f'digraph {{rankdir={direction};a->b;a->c;}}', 'ellipse-' + direction)
            byid = {v['_gvid']: v for v in nodes(g).values()}
            for e in g['edges']:
                for p, nid in zip(ends(e), (e['tail'], e['head'])):
                    n = byid[nid]; x, y = point(n['pos'])
                    value = ((p[0]-x)/(float(n['width'])*36))**2 + ((p[1]-y)/(float(n['height'])*36))**2
                    self.assertAlmostEqual(value, 1, delta=.07)
            assert_geometry(self, g)

    def test_arrowless_connectivity(self):
        for source in ('digraph {node[shape=box];a->b[dir=none];}',
                       'graph {node[shape=box];a--b;}'):
            g = render(source, 'arrowless')
            ns = nodes(g); end = ends(g['edges'][0])[1]
            self.assertAlmostEqual(end[1], box(ns['b'])[3], delta=.6)
            self.assertNotIn('_hdraw_', g['edges'][0])

    def test_arrows(self):
        for size in (0.5, 1, 3):
            g = render(f'digraph {{node[shape=box];a->b[dir=both,arrowsize={size}];}}', f'arrows-{size}')
            e = g['edges'][0]; start, end = ends(e)
            for key, p in (('_tdraw_', start), ('_hdraw_', end)):
                triangle = next(op['points'] for op in e[key] if op['op'] == 'P')
                tip = triangle[1]
                self.assertAlmostEqual(tip[0], p[0], delta=.05)
                self.assertLess(abs(tip[1]-p[1]), 2)
            tail = next(op['points'] for op in e['_tdraw_'] if op['op'] == 'P')
            self.assertLess(tail[0][1], tail[1][1])
            self.assertLess(tail[2][1], tail[1][1])
            head = next(op['points'] for op in e['_hdraw_'] if op['op'] == 'P')
            self.assertGreater(head[0][1], head[1][1])
            self.assertGreater(head[2][1], head[1][1])
        for arrow in ('vee', 'diamond', 'dot', 'odiamond', 'normalnormal'):
            g = render(f'digraph {{a->b[arrowhead={arrow},arrowsize=2];}}', 'arrow-' + arrow)
            self.assertIn('_hdraw_', g['edges'][0])

    def test_ports(self):
        for direction in ('TB', 'LR', 'BT', 'RL'):
            g = render(f'digraph {{rankdir={direction};node[shape=record];a[label="<l>left|<r>right"];b[label="<l>left|<r>right"];a:l:w->b:r:e;}}', 'ports-' + direction)
            ns = nodes(g); start, end = ends(g['edges'][0])
            self.assertAlmostEqual(start[0], box(ns['a'])[0], delta=.6)
            self.assertAlmostEqual(end[0], box(ns['b'])[2], delta=.6)
            assert_geometry(self, g)
        g = render('digraph {a[shape=plain,label=<<TABLE BORDER="1"><TR><TD PORT="p">left</TD><TD PORT="q">right</TD></TR></TABLE>>];b[shape=box];a:q:e->b:w;}', 'html-port')
        self.assertGreater(ends(g['edges'][0])[0][0], point(nodes(g)['a']['pos'])[0])
        assert_geometry(self, g)

    def test_variable_height_rank_sets(self):
        g=render('digraph {{rank=same;a;b;}a[height=2];a->c;b->c;}', 'same-variable-height')
        ns=nodes(g)
        self.assertAlmostEqual(point(ns['a']['pos'])[1],point(ns['b']['pos'])[1],delta=.01)
        assert_geometry(self,g)

    def test_rank_sets(self):
        for direction in ('TB', 'LR', 'BT', 'RL'):
            g = render(f'digraph {{rankdir={direction};{{rank=same;a;b;}}a->b;a->c;b->c;}}', 'same-' + direction)
            axis = 0 if direction in ('LR', 'RL') else 1
            ns = nodes(g)
            self.assertAlmostEqual(point(ns['a']['pos'])[axis], point(ns['b']['pos'])[axis], delta=.01)
            assert_geometry(self, g)
        for minimum in ('min', 'source'):
            for maximum in ('max', 'sink'):
                g = render(f'digraph {{{{rank={minimum};a;}}{{rank={maximum};z;}} b->c;z->b;c->a;}}', minimum + '-' + maximum)
                ys = {k:point(n['pos'])[1] for k,n in nodes(g).items()}
                self.assertGreaterEqual(ys['a'], max(ys.values()))
                self.assertLessEqual(ys['z'], min(ys.values()))
                if minimum == 'source': self.assertGreater(ys['a'], ys['b'])
                if maximum == 'sink': self.assertLess(ys['z'], ys['c'])
                assert_geometry(self, g)

    def test_constraints_do_not_infer_feedback_from_ignored_edges(self):
        g=render('digraph {a->b[constraint=false];b->a;}', 'ignored-cycle')
        ns=nodes(g)
        self.assertGreater(point(ns['b']['pos'])[1],point(ns['a']['pos'])[1])
        assert_geometry(self,g)

    def test_incoming_arrow_silhouettes_and_stems(self):
        for direction in ['TB', 'BT', 'LR', 'RL']:
            for scale in [0.5, 1, 2]:
                src = f'digraph {{rankdir={direction};node[shape=box];edge[arrowsize={scale},penwidth=2];a->c;b->c;d->c;}}'
                g = render(src, f'arrow-spacing-{direction}-{scale}')
                silhouettes = []
                for e in g['edges']:
                    for op in e.get('_hdraw_', []):
                        if op['op'] != 'P':
                            continue
                        xs, ys = zip(*op['points'])
                        silhouettes.append((min(xs), min(ys), max(xs), max(ys)))
                self.assertEqual(len(silhouettes), 3)
                for i, a in enumerate(silhouettes):
                    for b in silhouettes[i+1:]:
                        gap = max(b[0]-a[2], a[0]-b[2], b[1]-a[3], a[1]-b[3])
                        self.assertGreaterEqual(gap, 5.9, 'incoming arrowheads need visible whitespace')
                assert_geometry(self, g)
        source = (ROOT / 'tests/compiler/llvm-nested.dot').read_text()
        g = render(source, 'llvm-incoming-stems')
        by_id = {n['_gvid']: n for n in nodes(g).values()}
        target = next(n for n in by_id.values() if n['label'].startswith('{16:'))
        incoming = [e for e in g['edges'] if e['head'] == target['_gvid']]
        self.assertEqual(len(incoming), 2)
        tips = [ends(e)[1] for e in incoming]
        self.assertGreaterEqual(abs(tips[0][0]-tips[1][0]), 18)
        for edge in incoming:
            pts = next(curves(edge))
            # The final vertical stem must extend beyond the arrow base;
            # its approach must have a rounded, tangent-continuous elbow.
            self.assertGreater(abs(pts[-1][1]-pts[-4][1]), 8)
            for i in range(3, len(pts)-1, 3):
                a = [pts[i][d]-pts[i-1][d] for d in range(2)]
                b = [pts[i+1][d]-pts[i][d] for d in range(2)]
                self.assertLess(abs(a[0]*b[1]-a[1]*b[0]), 1.0)
                self.assertGreaterEqual(sum(x*y for x,y in zip(a,b)), -0.01)

    def test_incoming_slots(self):
        g=render('digraph {node[shape=box];a->c;b->c;d->c;}', 'incoming-slots')
        end_x=[ends(e)[1][0] for e in g['edges']]
        self.assertEqual(len(end_x),len(set(end_x)))
        assert_geometry(self,g)

    def test_combined_random_graphs(self):
        for seed in range(20):
            rng=random.Random(seed);direction=('TB','LR','BT','RL')[seed%4]
            clusters=''.join(f'subgraph cluster_{group}{{label="Group {group}";'+''.join(f'n{i};' for i in range(8) if i%2==group)+'}' for group in range(2))
            edges=[]
            for i in range(8):
                for j in range(i+1,8):
                    if rng.random()<.25:
                        attrs='label="edge text",' if rng.random()<.4 else ''
                        attrs+='constraint=false,' if rng.random()<.2 else ''
                        attrs+=f'minlen={rng.randrange(1,3)}'
                        edges.append(f'n{i}->n{j}[{attrs}];')
            edges.append('n6->n0;')
            src=f'digraph {{rankdir={direction};node[shape=box];'+clusters+'{rank=same;n2;n3;}'+''.join(edges)+'}'
            g=render(src,f'combined-{seed}')
            assert_geometry(self,g)
            self.assertEqual(g,render(src,f'combined-{seed}-repeat'))

    def test_edge_constraints(self):
        g = render('digraph {a->b[constraint=false];}', 'constraint-false')
        ns = nodes(g)
        self.assertAlmostEqual(point(ns['a']['pos'])[1], point(ns['b']['pos'])[1])
        g = render('digraph {a->b[minlen=4];a->c;c->d;}', 'minlen')
        ns = nodes(g)
        self.assertGreater(point(ns['d']['pos'])[1], point(ns['b']['pos'])[1])
        assert_geometry(self, g)

    def test_small_spacing_keeps_arrows_clear(self):
        g=render('digraph {ranksep=.02;node[shape=box];a->b[dir=both,arrowhead=normalnormal,arrowsize=3];}','small-spacing-arrows')
        ns=nodes(g);e=g['edges'][0]
        for key,other in (('_hdraw_','a'),('_tdraw_','b')):
            l,b,r,t=box(ns[other])
            for op in e[key]:
                for x,y in op.get('points',[]):
                    self.assertFalse(l<x<r and b<y<t,'arrow overlaps opposite node')
        assert_geometry(self,g)

    def test_spacing(self):
        small = nodes(render('digraph {nodesep=.1;ranksep=.1;a->b;a->c;}', 'spacing-small'))
        large = nodes(render('digraph {nodesep=3;ranksep=3;a->b;a->c;}', 'spacing-large'))
        self.assertGreater(point(large['c']['pos'])[0]-point(large['b']['pos'])[0],
                           point(small['c']['pos'])[0]-point(small['b']['pos'])[0]+150)
        self.assertGreater(point(large['a']['pos'])[1]-point(large['b']['pos'])[1],
                           point(small['a']['pos'])[1]-point(small['b']['pos'])[1]+150)
        g = render('digraph {ranksep="1 equally";a[height=2];a->b->c;}', 'equally')
        ns=nodes(g);a,b,c=[point(ns[n]['pos'])[1] for n in ('a','b','c')]
        self.assertAlmostEqual(a-b,b-c,delta=.1)

    def test_cluster_modes(self):
        for mode in ('none','global'):
            g=render(f'digraph {{clusterrank={mode};subgraph cluster_one {{a;b;}}a->b;}}','cluster-mode-'+mode)
            cluster=next(o for o in g['objects'] if o['name']=='cluster_one')
            self.assertNotIn('bb',cluster)
        g=render('digraph {subgraph group {cluster=true;label="Explicit";a;b;}a->b;}','explicit-cluster')
        cluster=next(o for o in g['objects'] if o['name']=='group')
        self.assertIn('bb',cluster)
        assert_geometry(self,g)

    def test_equal_spacing_with_cluster_boundaries(self):
        g=render('digraph {ranksep="1 equally";subgraph cluster_middle {label="Middle";b;c;}a->b->c->d;}','equally-clusters')
        ys=[point(nodes(g)[n]['pos'])[1] for n in ('a','b','c','d')]
        steps=[ys[i]-ys[i+1] for i in range(3)]
        self.assertLess(max(steps)-min(steps),.1)
        assert_geometry(self,g)

    def test_clusters(self):
        for direction in ('TB','LR','BT','RL'):
            for loc in ('t','b'):
                src=f'digraph {{rankdir={direction};subgraph cluster_outer{{label="Outer";labelloc={loc};a;subgraph cluster_inner{{label="Inner";labelloc={loc};b;c;}}}}subgraph cluster_other{{label="Other";d;}}a->b;b->c;c->d;a->d;}}'
                g=render(src,'clusters-'+direction+'-'+loc)
                objects={n['name']:n for n in g['objects']}
                for cname,members in [('cluster_outer',['a','b','c']),('cluster_inner',['b','c']),('cluster_other',['d'])]:
                    cl=objects[cname];bb=tuple(map(float,cl['bb'].split(',')))
                    self.assertIn('_draw_',cl);self.assertIn('_ldraw_',cl)
                    for member in members:
                        l,b,r,t=box(objects[member]);self.assertLessEqual(bb[0],l);self.assertLessEqual(bb[1],b);self.assertGreaterEqual(bb[2],r);self.assertGreaterEqual(bb[3],t)
                    lp=point(cl['lp']);lh=float(cl['lheight'])*72
                    for member in members:
                        l,b,r,t=box(objects[member]);self.assertFalse(l<lp[0]<r and b-lh/2<lp[1]<t+lh/2, 'cluster label overlaps node')
                assert_geometry(self,g)
        g=render('digraph {subgraph cluster_empty {label="Empty";}}','empty-cluster')
        self.assertIn('bb',g['objects'][0])

    def test_parallel_edges(self):
        g=render('digraph {node[shape=box];a->b;a->b;a->b;b->c;}', 'parallel')
        paths=[e['pos'] for e in g['edges'] if e['tail']==nodes(g)['a']['_gvid']]
        self.assertEqual(len(paths),len(set(paths)))
        ends_x=[ends(e)[1][0] for e in g['edges'] if e['tail']==nodes(g)['a']['_gvid']]
        self.assertEqual(len(ends_x),len(set(ends_x)))
        assert_geometry(self,g)

    def test_graph_labels_and_formats(self):
        for loc in ('t','b'):
            g=render(f'digraph {{label="Graph title";labelloc={loc};a->b[label="Edge"];}}','graph-label-'+loc)
            assert_geometry(self,g)
            ns=nodes(g);start,end=ends(g['edges'][0]);
            self.assertLess(abs(start[1]-box(ns['a'])[1]),4)
            self.assertLess(abs(end[1]-box(ns['b'])[3]),4)
        for fmt in ('svg','png','pdf','json','xdot','dot','plain'):
            self.assertTrue(render('digraph {subgraph cluster_g {label="Group";a;b;}a->b;}', 'format-'+fmt,fmt))

    def test_compound_edges(self):
        for direction in ('TB','LR','BT','RL'):
            g=render(f'digraph {{rankdir={direction};compound=true;subgraph cluster_a {{label="First";a;}}subgraph cluster_b {{label="Second";b;}}a->b[ltail=cluster_a,lhead=cluster_b];}}', 'compound-'+direction)
            objects={o['name']:o for o in g['objects']}
            for p,cname in zip(ends(g['edges'][0]),('cluster_a','cluster_b')):
                l,b,r,t=map(float,objects[cname]['bb'].split(','))
                self.assertLess(min(abs(p[0]-l),abs(p[0]-r),abs(p[1]-b),abs(p[1]-t)),.6)
                self.assertTrue(l-.6<=p[0]<=r+.6 and b-.6<=p[1]<=t+.6)
            assert_geometry(self,g)

    def test_dense_labels(self):
        g=render('digraph {node[shape=box];a->b[label="long label one",headlabel="HEAD",taillabel="TAIL",xlabel="EXTERNAL"];a->b[label="long label two"];a->c[label="long label three"];c->b[label="long label four"];}', 'labels')
        assert_geometry(self,g)
        ns=nodes(g);labels=[]
        for e in g['edges']:
            for key in ('_ldraw_','_hldraw_','_tldraw_'):
                for op in e.get(key,[]):
                    if op['op']=='T':
                        x,y=op['pt'];w=op['width'];labels.append((x-w/2,y-3,x+w/2,y+12))
        for i,r in enumerate(labels):
            for other in labels[i+1:]+[box(n) for n in ns.values()]:
                self.assertFalse(r[0]<other[2] and r[2]>other[0] and r[1]<other[3] and r[3]>other[1],f'label overlap {r}, {other}')


if __name__ == '__main__':
    unittest.main(verbosity=2)
