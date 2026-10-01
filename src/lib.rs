// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Layered-graph layout ported to Rust from iongraph's generic-layout
// (essence.ts) by Ben Visness — https://github.com/mozilla-spidermonkey/iongraph
// See NOTICE.md for attribution details.
//
// This file is the C ABI surface consumed by the Graphviz plugin
// (plugin/gvplugin_ion.c). The actual layout algorithm lives in core.rs.

pub mod compat;
pub mod core;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct IonPoint {
    pub x: f64,
    pub y: f64,
}

/// Per-edge route descriptor. `offset` indexes into the shared points buffer
/// returned via `route_points`; the route occupies `point_count` consecutive
/// points (cubic bezier control points, 3k+1 of them). `point_count == 0`
/// means the edge has no drawable route.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct IonRoute {
    pub offset: usize,
    pub point_count: usize,
    pub arrow_tip: IonPoint,
}

/// Compute the layout for a graph handed over from the C plugin.
///
/// `widths`/`heights` are input+output: the caller passes label sizes in and
/// receives the (clamped) cell sizes the layout actually used, so the plugin
/// can size the rendered node boxes to match.
///
/// `routes` receives one descriptor per input edge, in input order. All route
/// points live in a single buffer returned through `route_points` /
/// `route_points_len`, which the caller must release with
/// `ion_layout_free_points` after copying.
///
/// # Safety
/// All pointers must be valid for the given counts (or null where allowed:
/// loop metadata arrays, route outputs, and the graph dimensions).
#[no_mangle]
pub unsafe extern "C" fn ion_layout_compute(
    node_count: usize,
    widths: *mut f64,
    heights: *mut f64,
    edge_count: usize,
    tails: *const usize,
    heads: *const usize,
    loop_depths: *const i32,
    loop_headers: *const u8,
    backedges: *const u8,
    orientation: u32,
    positions: *mut IonPoint,
    routes: *mut IonRoute,
    route_points: *mut *mut IonPoint,
    route_points_len: *mut usize,
    graph_width: *mut f64,
    graph_height: *mut f64,
) -> i32 {
    ion_layout_compute_ex(
        node_count,
        widths,
        heights,
        edge_count,
        tails,
        heads,
        loop_depths,
        loop_headers,
        backedges,
        orientation,
        positions,
        routes,
        route_points,
        route_points_len,
        graph_width,
        graph_height,
        std::ptr::null(),
        std::ptr::null_mut(),
        std::ptr::null_mut(),
        0,
        std::ptr::null(),
    )
}

/// Extended layout entry point. Constraint arrays have the same counts as
/// nodes/edges; cluster records are preorder and receive their layout boxes.
/// # Safety
/// As for ion_layout_compute; optional arrays must be null or valid for counts.
#[no_mangle]
pub unsafe extern "C" fn ion_layout_compute_ex(
    node_count: usize,
    widths: *mut f64,
    heights: *mut f64,
    edge_count: usize,
    tails: *const usize,
    heads: *const usize,
    loop_depths: *const i32,
    loop_headers: *const u8,
    backedges: *const u8,
    orientation: u32,
    positions: *mut IonPoint,
    routes: *mut IonRoute,
    route_points: *mut *mut IonPoint,
    route_points_len: *mut usize,
    graph_width: *mut f64,
    graph_height: *mut f64,
    node_constraints: *const compat::NodeConstraint,
    edge_constraints: *mut compat::EdgeConstraint,
    clusters: *mut compat::Cluster,
    cluster_count: usize,
    options: *const compat::Options,
) -> i32 {
    if !route_points.is_null() {
        *route_points = std::ptr::null_mut();
    }
    if !route_points_len.is_null() {
        *route_points_len = 0;
    }
    if node_count > 0 && (widths.is_null() || heights.is_null() || positions.is_null()) {
        return -1;
    }
    if edge_count > 0 && (tails.is_null() || heads.is_null()) {
        return -1;
    }

    let widths = if node_count == 0 {
        &mut [][..]
    } else {
        std::slice::from_raw_parts_mut(widths, node_count)
    };
    let heights = if node_count == 0 {
        &mut [][..]
    } else {
        std::slice::from_raw_parts_mut(heights, node_count)
    };
    let tail_ids = if edge_count == 0 {
        &[][..]
    } else {
        std::slice::from_raw_parts(tails, edge_count)
    };
    let head_ids = if edge_count == 0 {
        &[][..]
    } else {
        std::slice::from_raw_parts(heads, edge_count)
    };
    let depths = if loop_depths.is_null() {
        &[][..]
    } else {
        std::slice::from_raw_parts(loop_depths, node_count)
    };
    let headers = if loop_headers.is_null() {
        &[][..]
    } else {
        std::slice::from_raw_parts(loop_headers, node_count)
    };
    let backs = if backedges.is_null() {
        &[][..]
    } else {
        std::slice::from_raw_parts(backedges, node_count)
    };
    let positions = if node_count == 0 {
        &mut [][..]
    } else {
        std::slice::from_raw_parts_mut(positions, node_count)
    };
    let routes = if routes.is_null() {
        &mut [][..]
    } else {
        std::slice::from_raw_parts_mut(routes, edge_count)
    };

    let nodes: Vec<core::NodeSpec> = (0..node_count)
        .map(|i| core::NodeSpec {
            width: widths[i],
            height: heights[i],
            loop_depth: depths.get(i).copied().unwrap_or(0),
            loop_header: headers.get(i).copied().unwrap_or(0) != 0,
            backedge: backs.get(i).copied().unwrap_or(0) != 0,
        })
        .collect();
    let edges: Vec<(usize, usize)> = tail_ids
        .iter()
        .zip(head_ids)
        .map(|(&t, &h)| (t, h))
        .collect();

    let orient = match orientation {
        1 => core::Orientation::LeftToRight,
        2 => core::Orientation::BottomToTop,
        3 => core::Orientation::RightToLeft,
        _ => core::Orientation::TopToBottom,
    };
    let result = if options.is_null() {
        core::layout_oriented(&nodes, &edges, orient)
    } else {
        if (node_count > 0 && node_constraints.is_null())
            || (edge_count > 0 && edge_constraints.is_null())
            || (cluster_count > 0 && clusters.is_null())
        {
            return -1;
        }
        let nc = if node_count == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(node_constraints, node_count)
        };
        let ec = if edge_count == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(edge_constraints, edge_count)
        };
        let cs = if cluster_count == 0 {
            &mut [][..]
        } else {
            std::slice::from_raw_parts_mut(clusters, cluster_count)
        };
        compat::layout(&nodes, &edges, orient, nc, ec, cs, *options)
    };

    if !edge_constraints.is_null() && edge_count > 0 {
        let ec = std::slice::from_raw_parts_mut(edge_constraints, edge_count);
        let mut channels = std::collections::HashMap::new();
        for (i, kind) in result.edge_kinds.iter().enumerate() {
            let (tail, head) = edges[i];
            let geometry_kind = if result.routes[i].points.is_empty()
                && tail < node_count
                && head < node_count
                && tail != head
            {
                if result.node_layers[head] < result.node_layers[tail]
                    || (result.node_layers[head] == result.node_layers[tail]
                        && *kind == core::EdgeKind::Feedback)
                {
                    core::EdgeKind::Feedback
                } else {
                    core::EdgeKind::Forward
                }
            } else {
                *kind
            };
            ec[i].kind = match geometry_kind {
                core::EdgeKind::Forward => 0,
                core::EdgeKind::Feedback => 1,
                core::EdgeKind::SelfLoop => 2,
                core::EdgeKind::Invalid => 3,
            };
            if geometry_kind == core::EdgeKind::Feedback {
                let next = channels.len() as u32;
                ec[i].channel = *channels.entry(edges[i].1).or_insert(next);
            }
        }
    }

    for i in 0..node_count {
        positions[i] = result.positions[i];
        let (w, h) = result.node_sizes[i];
        if w > 0.0 {
            widths[i] = w;
        }
        if h > 0.0 {
            heights[i] = h;
        }
    }

    if !routes.is_empty() && !route_points.is_null() && !route_points_len.is_null() {
        let total: usize = result.routes.iter().map(|r| r.points.len()).sum();
        let mut buf: Vec<IonPoint> = Vec::with_capacity(total);
        for (i, route) in result.routes.iter().enumerate().take(routes.len()) {
            routes[i] = IonRoute {
                offset: buf.len(),
                point_count: route.points.len(),
                arrow_tip: route.arrow_tip,
            };
            buf.extend_from_slice(&route.points);
        }
        let boxed = buf.into_boxed_slice();
        *route_points_len = boxed.len();
        *route_points = Box::into_raw(boxed) as *mut IonPoint;
    }

    if !graph_width.is_null() {
        *graph_width = result.width;
    }
    if !graph_height.is_null() {
        *graph_height = result.height;
    }
    0
}

/// Release a points buffer returned by `ion_layout_compute`.
///
/// # Safety
/// `points`/`len` must be exactly the values returned through
/// `route_points`/`route_points_len`, and must not be freed twice.
#[no_mangle]
pub unsafe extern "C" fn ion_layout_free_points(points: *mut IonPoint, len: usize) {
    if points.is_null() || len == 0 {
        return;
    }
    drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
        points, len,
    )));
}

/// Route between escape points outside obstacle boxes. The caller prepends
/// and appends its shape/port attachment segments before native clipping.
/// # Safety
/// Boxes must have box_count elements; output pointers must be valid.
#[no_mangle]
pub unsafe extern "C" fn ion_route_compute(
    start: IonPoint,
    end: IonPoint,
    boxes: *const compat::Rect,
    box_count: usize,
    lane: f64,
    points: *mut *mut IonPoint,
    len: *mut usize,
) -> i32 {
    ion_route_via_compute(&start, 1, end, boxes, box_count, lane, points, len)
}

/// Route through mandatory waypoints, rounding the joined path as a whole.
/// # Safety
/// Waypoints and boxes must contain their stated element counts; output
/// pointers must be valid. Returned points use ion_layout_free_points.
#[no_mangle]
pub unsafe extern "C" fn ion_route_via_compute(
    waypoints: *const IonPoint,
    waypoint_count: usize,
    end: IonPoint,
    boxes: *const compat::Rect,
    box_count: usize,
    lane: f64,
    points: *mut *mut IonPoint,
    len: *mut usize,
) -> i32 {
    ion_route_attached_compute(waypoints, waypoint_count, end, std::ptr::null(),
                               boxes, box_count, lane, points, len)
}

/// Route and round the attachment segments together with the channel.
/// # Safety
/// Attachments must be null or contain two points (tail/head aim). Other
/// pointers obey the same contract as ion_route_via_compute.
#[no_mangle]
pub unsafe extern "C" fn ion_route_attached_compute(
    waypoints: *const IonPoint,
    waypoint_count: usize,
    end: IonPoint,
    attachments: *const IonPoint,
    boxes: *const compat::Rect,
    box_count: usize,
    lane: f64,
    points: *mut *mut IonPoint,
    len: *mut usize,
) -> i32 {
    if points.is_null() || len.is_null() {
        return -1;
    }
    *points = std::ptr::null_mut();
    *len = 0;
    let boxes = if box_count == 0 {
        &[][..]
    } else {
        if boxes.is_null() {
            return -1;
        }
        std::slice::from_raw_parts(boxes, box_count)
    };
    if waypoints.is_null() || waypoint_count == 0 {
        return -1;
    }
    let mut via = std::slice::from_raw_parts(waypoints, waypoint_count).to_vec();
    via.push(end);
    let aims = if attachments.is_null() { None } else {
        Some([*attachments, *attachments.add(1)])
    };
    let Some(path) = compat::route_via_attached(&via, boxes, lane.max(2.0), aims) else {
        return -1;
    };
    let buf = path.into_boxed_slice();
    *len = buf.len();
    *points = Box::into_raw(buf) as *mut IonPoint;
    0
}
