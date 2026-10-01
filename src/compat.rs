// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Graphviz constraints on top of Ion's preferred ordering and loop columns.
//! The default layout remains the original pipeline. Explicit rank and group
//! constraints are solved before placing rows; routes are rebuilt afterwards.
use crate::{
    core::{self, LayoutResult, NodeSpec, Orientation},
    IonPoint,
};
use std::cmp::Ordering;
use std::collections::{BinaryHeap, HashMap, VecDeque};

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct NodeConstraint {
    pub group: usize,
    pub kind: u32,
    pub cluster: usize,
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct EdgeConstraint {
    pub minlen: u32,
    pub constraint: u32,
    pub weight: f64,
    pub kind: u32,
    pub channel: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Rect {
    pub x0: f64,
    pub y0: f64,
    pub x1: f64,
    pub y1: f64,
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Cluster {
    pub parent: usize,
    pub margin: f64,
    pub label_w: f64,
    pub label_h: f64,
    pub bounds: Rect,
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Options {
    pub nodesep: f64,
    pub ranksep: f64,
    pub ranksep_equally: u32,
    pub arrow_clearance: f64,
}

fn root(parent: &mut [usize], mut n: usize) -> usize {
    let mut r = n;
    while parent[r] != r {
        r = parent[r];
    }
    while parent[n] != n {
        let p = parent[n];
        parent[n] = r;
        n = p;
    }
    r
}
fn unite(parent: &mut [usize], a: usize, b: usize) {
    let a = root(parent, a);
    let b = root(parent, b);
    parent[b] = a;
}
fn reachable(adj: &[Vec<(usize, usize)>], start: usize, end: usize) -> bool {
    let mut seen = vec![false; adj.len()];
    let mut work = vec![start];
    while let Some(n) = work.pop() {
        if n == end {
            return true;
        }
        if seen[n] {
            continue;
        }
        seen[n] = true;
        work.extend(adj[n].iter().map(|&(v, _)| v));
    }
    false
}

pub fn layout(
    nodes: &[NodeSpec],
    edges: &[(usize, usize)],
    orient: Orientation,
    nc: &[NodeConstraint],
    ec: &[EdgeConstraint],
    clusters: &mut [Cluster],
    opts: Options,
) -> LayoutResult {
    let mut cfg = core::CFG;
    if opts.nodesep >= 0.0 {
        cfg.gap = opts.nodesep;
    }
    if opts.ranksep >= 0.0 {
        cfg.track_pad = opts.ranksep / 2.0;
    }
    cfg.gap = cfg.gap.max(opts.arrow_clearance);
    cfg.track_pad = cfg.track_pad.max(opts.arrow_clearance / 2.0);
    let mut result = core::layout_oriented_with_config(nodes, edges, orient, cfg);
    let horizontal = matches!(orient, Orientation::LeftToRight | Orientation::RightToLeft);
    let reversed = matches!(orient, Orientation::BottomToTop | Orientation::RightToLeft);
    let ranking = nc.iter().any(|n| n.group != usize::MAX || n.kind != 0)
        || ec.iter().any(|e| e.minlen != 1 || e.constraint == 0)
        || opts.ranksep_equally != 0;
    if !ranking && clusters.is_empty() {
        return result;
    }
    let count = nodes.len();
    let mut ranks = result.node_layers.clone();
    if ranking {
        let mut parent: Vec<usize> = (0..count).collect();
        let mut groups = HashMap::new();
        let mut min = None;
        let mut max = None;
        for (i, n) in nc.iter().enumerate() {
            if n.group != usize::MAX {
                if let Some(&prev) = groups.get(&n.group) {
                    unite(&mut parent, i, prev);
                } else {
                    groups.insert(n.group, i);
                }
            }
            if n.kind == 1 || n.kind == 2 {
                if let Some(m) = min {
                    unite(&mut parent, i, m);
                } else {
                    min = Some(i);
                }
            }
            if n.kind == 3 || n.kind == 4 {
                if let Some(m) = max {
                    unite(&mut parent, i, m);
                } else {
                    max = Some(i);
                }
            }
        }
        let ids: Vec<_> = (0..count).map(|i| root(&mut parent, i)).collect();
        let min = min.map(|i| ids[i]);
        let max = max.map(|i| ids[i]);
        let source = nc.iter().any(|n| n.kind == 2);
        let sink = nc.iter().any(|n| n.kind == 4);
        let mut adj = vec![Vec::<(usize, usize)>::new(); count];
        // Rank sets take precedence over edges. Feedback edges do not impose
        // impossible inequalities; neither do edges entering min/leaving max.
        let mut candidates: Vec<_> = edges
            .iter()
            .enumerate()
            .filter(|(i, _)| ec[*i].constraint != 0)
            .collect();
        candidates.sort_by(|(a, _), (b, _)| ec[*b].weight.total_cmp(&ec[*a].weight).then(a.cmp(b)));
        for (i, &(a, b)) in candidates {
            if a >= count || b >= count || a == b {
                continue;
            }
            let (a, b) = (ids[a], ids[b]);
            if a == b || min == Some(b) || max == Some(a) {
                continue;
            }
            if !reachable(&adj, b, a) {
                adj[a].push((b, ec[i].minlen as usize));
            }
        }
        for &id in &ids {
            if let Some(m) = min {
                if m != id && !reachable(&adj, id, m) {
                    adj[m].push((id, usize::from(source)));
                }
            }
            if let Some(m) = max {
                if m != id && !reachable(&adj, m, id) {
                    adj[id].push((m, usize::from(sink)));
                }
            }
        }
        let mut indegree = vec![0; count];
        for list in &adj {
            for &(v, _) in list {
                indegree[v] += 1;
            }
        }
        let mut queue: VecDeque<_> = (0..count).filter(|&i| indegree[i] == 0).collect();
        let mut layers = vec![0usize; count];
        while let Some(a) = queue.pop_front() {
            for &(b, len) in &adj[a] {
                layers[b] = layers[b].max(layers[a].saturating_add(len));
                indegree[b] -= 1;
                if indegree[b] == 0 {
                    queue.push_back(b);
                }
            }
        }
        ranks = ids.iter().map(|&i| layers[i]).collect();
    }
    let max_rank = *ranks.iter().max().unwrap_or(&0);
    let mut row_h = vec![0.0f64; max_rank + 1];
    for (i, &r) in ranks.iter().enumerate() {
        row_h[r] = row_h[r].max(if horizontal {
            result.node_sizes[i].0
        } else {
            result.node_sizes[i].1
        });
    }
    if opts.ranksep_equally != 0 {
        let h = row_h.iter().copied().fold(0.0, f64::max);
        row_h.fill(h);
    }
    let gap = cfg.gap;
    let rankgap = if opts.ranksep >= 0.0 {
        opts.ranksep.max(opts.arrow_clearance)
    } else {
        2.0 * cfg.track_pad + cfg.track_step
    };
    // Reserve borders at cluster entry/exit ranks, rather than inserting
    // every enclosing border between every pair of internal ranks.
    let mut first = vec![usize::MAX; clusters.len()];
    let mut last = vec![0; clusters.len()];
    for (i, n) in nc.iter().enumerate() {
        if n.cluster != usize::MAX {
            first[n.cluster] = first[n.cluster].min(ranks[i]);
            last[n.cluster] = last[n.cluster].max(ranks[i]);
        }
    }
    for c in (0..clusters.len()).rev() {
        if first[c] == usize::MAX {
            first[c] = 0;
        }
        let parent = clusters[c].parent;
        if parent != usize::MAX {
            first[parent] = first[parent].min(first[c]);
            last[parent] = last[parent].max(last[c]);
        }
    }
    let mut before = vec![0.0f64; row_h.len()];
    let mut after = vec![0.0f64; row_h.len()];
    for c in 0..clusters.len() {
        let mut top = 0.0;
        let mut bottom = 0.0;
        let mut ancestor = c;
        loop {
            let pad = clusters[ancestor].margin
                + if horizontal {
                    0.0
                } else {
                    clusters[ancestor].label_h + 8.0
                };
            if first[ancestor] == first[c] {
                top += pad;
            }
            if last[ancestor] == last[c] {
                bottom += pad;
            }
            ancestor = clusters[ancestor].parent;
            if ancestor == usize::MAX {
                break;
            }
        }
        before[first[c]] = before[first[c]].max(top);
        after[last[c]] = after[last[c]].max(bottom);
    }
    let mut row_y = vec![cfg.padding + before[0]; row_h.len()];
    let equal_gap = (1..row_h.len())
        .map(|r| rankgap + after[r - 1] + before[r])
        .fold(rankgap, f64::max);
    for r in 1..row_h.len() {
        let spacing = if opts.ranksep_equally != 0 {
            equal_gap
        } else {
            rankgap + after[r - 1] + before[r]
        };
        row_y[r] = row_y[r - 1] + row_h[r - 1] + spacing;
    }
    let mut cross = vec![0.0; count];
    if clusters.is_empty() {
        for r in 0..row_h.len() {
            let mut row: Vec<_> = (0..count).filter(|&i| ranks[i] == r).collect();
            row.sort_by(|&a, &b| {
                let pa = result.positions[a];
                let pb = result.positions[b];
                (if horizontal { pa.y } else { pa.x })
                    .total_cmp(&(if horizontal { pb.y } else { pb.x }))
                    .then(a.cmp(&b))
            });
            let mut right = cfg.padding;
            for i in row {
                let size = if horizontal {
                    result.node_sizes[i].1
                } else {
                    result.node_sizes[i].0
                };
                let old = if horizontal {
                    result.positions[i].y
                } else {
                    result.positions[i].x
                };
                cross[i] = old.max(right + size / 2.0);
                right = cross[i] + size / 2.0 + gap;
            }
        }
    } else {
        // Each cluster owns a perpendicular interval spanning its entire
        // rank range. Sibling intervals cannot interleave; recursively packed
        // children preserve containment, even for cross-cluster rank sets.
        struct Packing<'a> {
            nc: &'a [NodeConstraint],
            cs: &'a mut [Cluster],
            sizes: &'a [(f64, f64)],
            ranks: &'a [usize],
            preferred: &'a [IonPoint],
            horizontal: bool,
            gap: f64,
            cross: &'a mut [f64],
            first: &'a [usize],
            last: &'a [usize],
        }
        impl Packing<'_> {
            fn preferred_left(&self, group: usize) -> f64 {
                let mut min = f64::INFINITY;
                for (i, n) in self.nc.iter().enumerate() {
                    let mut member = n.cluster;
                    while member != group && member != usize::MAX {
                        member = self.cs[member].parent;
                    }
                    if member == group {
                        min = min.min(if self.horizontal {
                            self.preferred[i].y - self.sizes[i].1 / 2.0
                        } else {
                            self.preferred[i].x - self.sizes[i].0 / 2.0
                        });
                    }
                }
                if min.is_finite() {
                    min
                } else {
                    0.0
                }
            }
            fn pack(&mut self, group: usize, left: f64) -> f64 {
                let margin = if group == usize::MAX {
                    0.0
                } else {
                    self.cs[group].margin
                };
                let top_extra = if group != usize::MAX && self.horizontal {
                    self.cs[group].label_h + 8.0
                } else {
                    0.0
                };
                let mut width = 0.0f64;
                let min_preferred = self.preferred_left(group);
                let max_rank = *self.ranks.iter().max().unwrap_or(&0);
                for r in 0..=max_rank {
                    let mut row: Vec<_> = (0..self.nc.len())
                        .filter(|&i| self.nc[i].cluster == group && self.ranks[i] == r)
                        .collect();
                    row.sort_by(|&a, &b| {
                        (if self.horizontal {
                            self.preferred[a].y
                        } else {
                            self.preferred[a].x
                        })
                        .total_cmp(
                            &(if self.horizontal {
                                self.preferred[b].y
                            } else {
                                self.preferred[b].x
                            }),
                        )
                        .then(a.cmp(&b))
                    });
                    let mut x = left + margin + top_extra;
                    for (j, i) in row.into_iter().enumerate() {
                        if j > 0 {
                            x += self.gap;
                        }
                        let w = if self.horizontal {
                            self.sizes[i].1
                        } else {
                            self.sizes[i].0
                        };
                        let old = if self.horizontal {
                            self.preferred[i].y - w / 2.0
                        } else {
                            self.preferred[i].x - w / 2.0
                        };
                        x = x.max(left + margin + top_extra + old - min_preferred);
                        self.cross[i] = x + w / 2.0;
                        x += w;
                    }
                    width = width.max(x - left - margin - top_extra);
                }
                let mut children: Vec<_> = (0..self.cs.len())
                    .filter(|&i| self.cs[i].parent == group)
                    .collect();
                children.sort_by(|&a, &b| {
                    self.preferred_left(a)
                        .total_cmp(&self.preferred_left(b))
                        .then(a.cmp(&b))
                });
                let overlap = self.nc.iter().enumerate().any(|(i, n)| {
                    n.cluster == group
                        && children.iter().any(|&child| {
                            self.first[child] <= self.ranks[i] && self.ranks[i] <= self.last[child]
                        })
                });
                let inner = left + margin + top_extra;
                let mut cursor = inner + if overlap { width + self.gap } else { 0.0 };
                let mut end = inner + width;
                for (j, child) in children.into_iter().enumerate() {
                    if j > 0 {
                        cursor += self.gap;
                    }
                    let preferred = inner + self.preferred_left(child) - min_preferred;
                    cursor = cursor.max(preferred);
                    cursor += self.pack(child, cursor);
                    end = end.max(cursor);
                }
                let label_min = if group == usize::MAX {
                    0.0
                } else if self.horizontal {
                    self.cs[group].label_h + 8.0
                } else {
                    self.cs[group].label_w + 8.0
                };
                let total = (end - left + margin + top_extra).max(2.0 * margin + label_min);
                if group != usize::MAX {
                    self.cs[group].bounds = Rect {
                        x0: left,
                        y0: 0.0,
                        x1: left + total,
                        y1: 0.0,
                    };
                }
                total
            }
        }
        Packing {
            nc,
            cs: clusters,
            sizes: &result.node_sizes,
            ranks: &ranks,
            preferred: &result.positions,
            horizontal,
            gap,
            cross: &mut cross,
            first: &first,
            last: &last,
        }
        .pack(usize::MAX, cfg.padding);
    }
    for i in 0..count {
        let flow = row_y[ranks[i]] + row_h[ranks[i]] / 2.0;
        result.positions[i] = if horizontal {
            IonPoint {
                x: flow,
                y: cross[i],
            }
        } else {
            IonPoint {
                x: cross[i],
                y: flow,
            }
        };
    }
    // Children precede their parents in this reverse walk (preorder input).
    for c in (0..clusters.len()).rev() {
        let mut lo = f64::INFINITY;
        let mut hi = f64::NEG_INFINITY;
        for (i, n) in nc.iter().enumerate() {
            if n.cluster == c {
                let flow = if horizontal {
                    result.positions[i].x
                } else {
                    result.positions[i].y
                };
                let h = if horizontal {
                    result.node_sizes[i].0
                } else {
                    result.node_sizes[i].1
                };
                lo = lo.min(flow - h / 2.0);
                hi = hi.max(flow + h / 2.0);
            }
        }
        for child in clusters.iter() {
            if child.parent == c {
                lo = lo.min(child.bounds.y0);
                hi = hi.max(child.bounds.y1);
            }
        }
        if !lo.is_finite() {
            lo = row_y[first[c]];
            hi = lo;
        }
        let margin = clusters[c].margin;
        let label = if horizontal {
            0.0
        } else {
            clusters[c].label_h + 8.0
        };
        clusters[c].bounds.y0 = lo - margin - label;
        clusters[c].bounds.y1 = hi + margin + label;
    }
    result.width = cfg.padding;
    result.height = cfg.padding;
    for (i, p) in result.positions.iter().enumerate() {
        result.width = result
            .width
            .max(p.x + result.node_sizes[i].0 / 2.0 + cfg.padding);
        result.height = result
            .height
            .max(p.y + result.node_sizes[i].1 / 2.0 + cfg.padding);
    }
    for c in clusters.iter_mut() {
        if horizontal {
            c.bounds = Rect {
                x0: c.bounds.y0,
                y0: c.bounds.x0,
                x1: c.bounds.y1,
                y1: c.bounds.x1,
            };
        }
        result.width = result.width.max(c.bounds.x1 + cfg.padding);
        result.height = result.height.max(c.bounds.y1 + cfg.padding);
    }
    if reversed {
        for p in &mut result.positions {
            if horizontal {
                p.x = result.width - p.x;
            } else {
                p.y = result.height - p.y;
            }
        }
        for c in clusters.iter_mut() {
            if horizontal {
                let x = c.bounds.x0;
                c.bounds.x0 = result.width - c.bounds.x1;
                c.bounds.x1 = result.width - x;
            } else {
                let y = c.bounds.y0;
                c.bounds.y0 = result.height - c.bounds.y1;
                c.bounds.y1 = result.height - y;
            }
        }
    }
    result.node_layers = ranks;
    // Geometry changed; stale splines must never escape to the renderer.
    result
        .routes
        .iter_mut()
        .for_each(|r| *r = core::Route::default());
    result
}

#[derive(Clone, Copy)]
struct State {
    cost: f64,
    index: usize,
}
impl PartialEq for State {
    fn eq(&self, b: &Self) -> bool {
        self.cost == b.cost && self.index == b.index
    }
}
impl Eq for State {}
impl PartialOrd for State {
    fn partial_cmp(&self, b: &Self) -> Option<Ordering> {
        Some(self.cmp(b))
    }
}
impl Ord for State {
    fn cmp(&self, b: &Self) -> Ordering {
        b.cost.total_cmp(&self.cost).then(b.index.cmp(&self.index))
    }
}

fn clear(a: IonPoint, b: IonPoint, boxes: &[Rect]) -> bool {
    boxes.iter().all(|r| {
        if (a.x - b.x).abs() < 1e-7 {
            a.x <= r.x0 + 1e-7
                || a.x >= r.x1 - 1e-7
                || a.y.max(b.y) <= r.y0 + 1e-7
                || a.y.min(b.y) >= r.y1 - 1e-7
        } else {
            a.y <= r.y0 + 1e-7
                || a.y >= r.y1 - 1e-7
                || a.x.max(b.x) <= r.x0 + 1e-7
                || a.x.min(b.x) >= r.x1 - 1e-7
        }
    })
}
/// Rectilinear visibility grid with Dijkstra search on (point, direction).
/// A bend penalty favors Ion-like long columns. Every segment is checked
/// against actual obstacles; there is no collision-ignoring fallback.
pub fn route(start: IonPoint, end: IonPoint, boxes: &[Rect], lane: f64) -> Option<Vec<IonPoint>> {
    let mut margin = lane.max(2.0) * 4.0;
    let inside = |p: IonPoint, r: &Rect| {
        p.x > r.x0 + 1e-7 && p.x < r.x1 - 1e-7 && p.y > r.y0 + 1e-7 && p.y < r.y1 - 1e-7
    };
    if boxes.iter().any(|r| inside(start, r) || inside(end, r)) {
        return None;
    }
    loop {
        let viewport = Rect {
            x0: start.x.min(end.x) - margin,
            y0: start.y.min(end.y) - margin,
            x1: start.x.max(end.x) + margin,
            y1: start.y.max(end.y) + margin,
        };
        let local: Vec<_> = boxes
            .iter()
            .copied()
            .filter(|r| {
                r.x0 <= viewport.x1
                    && r.x1 >= viewport.x0
                    && r.y0 <= viewport.y1
                    && r.y1 >= viewport.y0
            })
            .collect();
        if let Some(path) = route_grid(start, end, &local, lane, viewport) {
            return Some(path);
        }
        if boxes.iter().all(|r| {
            r.x0 >= viewport.x0 && r.x1 <= viewport.x1 && r.y0 >= viewport.y0 && r.y1 <= viewport.y1
        }) {
            return None;
        }
        margin *= 2.0;
        if !margin.is_finite() {
            return None;
        }
    }
}

fn route_grid(
    start: IonPoint,
    end: IonPoint,
    boxes: &[Rect],
    lane: f64,
    viewport: Rect,
) -> Option<Vec<IonPoint>> {
    let mut xs = vec![start.x, end.x];
    let mut ys = vec![start.y, end.y];
    for r in boxes {
        xs.extend([r.x0 - lane, r.x1 + lane]);
        ys.extend([r.y0 - lane, r.y1 + lane]);
    }
    xs.retain(|&x| x > viewport.x0 && x < viewport.x1);
    ys.retain(|&y| y > viewport.y0 && y < viewport.y1);
    xs.extend([viewport.x0, viewport.x1]);
    ys.extend([viewport.y0, viewport.y1]);
    xs.sort_by(f64::total_cmp);
    ys.sort_by(f64::total_cmp);
    xs.dedup();
    ys.dedup();
    let nx = xs.len();
    let total = nx.checked_mul(ys.len())?.checked_mul(2)?;
    let sx = xs.binary_search_by(|v| v.total_cmp(&start.x)).ok()?;
    let sy = ys.binary_search_by(|v| v.total_cmp(&start.y)).ok()?;
    let ex = xs.binary_search_by(|v| v.total_cmp(&end.x)).ok()?;
    let ey = ys.binary_search_by(|v| v.total_cmp(&end.y)).ok()?;
    let target = ey * nx + ex;
    let origin = sy * nx + sx;
    let mut dist = vec![f64::INFINITY; total];
    let mut prev = vec![usize::MAX; total];
    let mut heap = BinaryHeap::new();
    for d in 0..2 {
        dist[origin * 2 + d] = 0.0;
        heap.push(State {
            cost: (start.x - end.x).abs() + (start.y - end.y).abs(),
            index: origin * 2 + d,
        });
    }
    let heuristic = |point: usize| (xs[point % nx] - end.x).abs() + (ys[point / nx] - end.y).abs();
    let mut finish = None;
    while let Some(State { cost, index }) = heap.pop() {
        if cost > dist[index] + heuristic(index / 2) {
            continue;
        }
        let point = index / 2;
        if point == target {
            finish = Some(index);
            break;
        }
        let (x, y) = (point % nx, point / nx);
        let a = IonPoint { x: xs[x], y: ys[y] };
        for (xx, yy, dir) in [
            (x.wrapping_sub(1), y, 0),
            (x + 1, y, 0),
            (x, y.wrapping_sub(1), 1),
            (x, y + 1, 1),
        ] {
            if xx >= nx || yy >= ys.len() {
                continue;
            }
            let b = IonPoint {
                x: xs[xx],
                y: ys[yy],
            };
            if !clear(a, b, boxes) {
                continue;
            }
            let next = (yy * nx + xx) * 2 + dir;
            let next_cost = dist[index]
                + (a.x - b.x).abs()
                + (a.y - b.y).abs()
                + if index % 2 == dir { 0.0 } else { 24.0 };
            if next_cost < dist[next] {
                dist[next] = next_cost;
                prev[next] = index;
                heap.push(State {
                    cost: next_cost + heuristic(next / 2),
                    index: next,
                });
            }
        }
    }
    let mut cur = finish?;
    let mut out = Vec::new();
    loop {
        let p = cur / 2;
        out.push(IonPoint {
            x: xs[p % nx],
            y: ys[p / nx],
        });
        if prev[cur] == usize::MAX {
            break;
        }
        cur = prev[cur];
    }
    out.reverse();
    let mut compact: Vec<IonPoint> = Vec::new();
    for p in out {
        if compact.last() == Some(&p) {
            continue;
        }
        if compact.len() > 1 {
            let a = compact[compact.len() - 2];
            let b = compact[compact.len() - 1];
            if ((a.x - b.x).abs() < 1e-7 && (b.x - p.x).abs() < 1e-7)
                || ((a.y - b.y).abs() < 1e-7 && (b.y - p.y).abs() < 1e-7)
            {
                compact.pop();
            }
        }
        compact.push(p);
    }
    Some(compact)
}

/// Route each channel leg and round their joined polyline, so mandatory
/// waypoint joins have the same smooth corners as bends within a leg.
pub fn route_via(waypoints: &[IonPoint], boxes: &[Rect], lane: f64) -> Option<Vec<IonPoint>> {
    route_via_attached(waypoints, boxes, lane, None)
}

/// Include the native-clipped attachment segments in corner rounding.
pub fn route_via_attached(waypoints: &[IonPoint], boxes: &[Rect], lane: f64,
                          attachments: Option<[IonPoint; 2]>) -> Option<Vec<IonPoint>> {
    let mut joined = Vec::new();
    if let Some(aims) = attachments {
        joined.push(aims[0]);
    }
    for (i, pair) in waypoints.windows(2).enumerate() {
        let part = route(pair[0], pair[1], boxes, lane)?;
        let skip = usize::from(i > 0);
        joined.extend_from_slice(&part[skip..]);
    }
    if let Some(aims) = attachments {
        joined.push(aims[1]);
    }
    Some(beziers(&joined, boxes))
}

/// Round corners within a certified empty square around every bend. The
/// radius cannot exceed the nearest obstacle's Chebyshev distance.
pub fn beziers(path: &[IonPoint], boxes: &[Rect]) -> Vec<IonPoint> {
    let Some(first) = path.first() else {
        return Vec::new();
    };
    let mut radius = core::CFG.radius;
    for p in path.iter().skip(1).take(path.len().saturating_sub(2)) {
        for r in boxes {
            let dx = (r.x0 - p.x).max(p.x - r.x1).max(0.0);
            let dy = (r.y0 - p.y).max(p.y - r.y1).max(0.0);
            radius = radius.min(dx.max(dy));
        }
    }
    let mut builder = core::Ortho::start(first.x, first.y);
    for p in path.iter().skip(1) {
        builder.to(p.x, p.y);
    }
    builder.beziers(radius)
}
