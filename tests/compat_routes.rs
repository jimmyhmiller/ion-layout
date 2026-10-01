// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

use ion_layout::{
    compat::{beziers, route, Rect},
    IonPoint,
};

fn assert_clear(path: &[IonPoint], obstacles: &[Rect]) {
    let curves = beziers(path, obstacles);
    assert_eq!((curves.len() - 1) % 3, 0);
    for i in (0..curves.len() - 1).step_by(3) {
        for sample in 0..=1000 {
            let t = sample as f64 / 1000.0;
            let u = 1.0 - t;
            let weights = [u * u * u, 3.0 * u * u * t, 3.0 * u * t * t, t * t * t];
            let x: f64 = (0..4).map(|j| weights[j] * curves[i + j].x).sum();
            let y: f64 = (0..4).map(|j| weights[j] * curves[i + j].y).sum();
            for r in obstacles {
                assert!(
                    !(x > r.x0 + 1e-6 && x < r.x1 - 1e-6 && y > r.y0 + 1e-6 && y < r.y1 - 1e-6),
                    "curve penetrated obstacle at {x},{y}"
                );
            }
        }
    }
}

#[test]
fn expands_viewport_to_go_around_long_barrier() {
    let boxes = [Rect {
        x0: -1.0,
        y0: -1000.0,
        x1: 1.0,
        y1: 1000.0,
    }];
    let start = IonPoint { x: -10.0, y: 0.0 };
    let end = IonPoint { x: 10.0, y: 0.0 };
    let path = route(start, end, &boxes, 4.0).unwrap();
    assert_eq!(path.first(), Some(&start));
    assert_eq!(path.last(), Some(&end));
    assert!(path.iter().any(|p| p.y.abs() >= 1000.0));
    assert_clear(&path, &boxes);
}

#[test]
fn enclosed_destination_has_no_collision_ignoring_fallback() {
    let boxes = [
        Rect {
            x0: -10.0,
            y0: -10.0,
            x1: 10.0,
            y1: -8.0,
        },
        Rect {
            x0: -10.0,
            y0: 8.0,
            x1: 10.0,
            y1: 10.0,
        },
        Rect {
            x0: -10.0,
            y0: -10.0,
            x1: -8.0,
            y1: 10.0,
        },
        Rect {
            x0: 8.0,
            y0: -10.0,
            x1: 10.0,
            y1: 10.0,
        },
    ];
    assert!(route(
        IonPoint { x: 30.0, y: 0.0 },
        IonPoint { x: 0.0, y: 0.0 },
        &boxes,
        4.0
    )
    .is_none());
}

#[test]
fn corner_rounding_is_clear_and_routes_are_deterministic() {
    let boxes = [
        Rect {
            x0: 0.0,
            y0: 0.0,
            x1: 50.0,
            y1: 50.0,
        },
        Rect {
            x0: 60.0,
            y0: -20.0,
            x1: 90.0,
            y1: 70.0,
        },
    ];
    let start = IonPoint { x: -10.0, y: 25.0 };
    let end = IonPoint { x: 100.0, y: 25.0 };
    let path = route(start, end, &boxes, 4.0).unwrap();
    assert_eq!(Some(path.clone()), route(start, end, &boxes, 4.0));
    assert_clear(&path, &boxes);
    let curves = beziers(&path, &boxes);
    assert!(curves
        .windows(4)
        .any(|p| (p[0].x - p[3].x).abs() > 1e-6 && (p[0].y - p[3].y).abs() > 1e-6));
}

#[test]
fn coincident_points_and_blocked_endpoints() {
    let p = IonPoint { x: 10.0, y: 20.0 };
    assert_eq!(route(p, p, &[], 4.0), Some(vec![p]));
    let boxes = [Rect {
        x0: 0.0,
        y0: 0.0,
        x1: 100.0,
        y1: 100.0,
    }];
    assert!(route(p, IonPoint { x: 120.0, y: 20.0 }, &boxes, 4.0).is_none());
}

#[test]
fn waypoint_channels_round_the_join_without_reversing() {
    use ion_layout::compat::route_via;
    let via = [IonPoint { x: 0.0, y: 100.0 }, IonPoint { x: 80.0, y: 100.0 },
               IonPoint { x: 80.0, y: 0.0 }, IonPoint { x: 0.0, y: 0.0 }];
    let curve = route_via(&via, &[], 4.0).unwrap();
    assert_eq!(curve.first(), via.first());
    assert_eq!(curve.last(), via.last());
    for i in (3..curve.len() - 1).step_by(3) {
        let incoming = (curve[i].x - curve[i-1].x, curve[i].y - curve[i-1].y);
        let outgoing = (curve[i+1].x - curve[i].x, curve[i+1].y - curve[i].y);
        let cross = incoming.0 * outgoing.1 - incoming.1 * outgoing.0;
        let dot = incoming.0 * outgoing.0 + incoming.1 * outgoing.1;
        assert!(cross.abs() < 1e-6, "channel join must have a continuous tangent");
        assert!(dot >= 0.0, "channel join must not reverse");
    }
}

#[test]
fn attachments_remove_retraced_escape_segments() {
    use ion_layout::compat::route_via_attached;
    let via = [IonPoint { x: 0.0, y: 80.0 }, IonPoint { x: 40.0, y: 80.0 },
               IonPoint { x: 40.0, y: 40.0 }];
    let aims = [IonPoint { x: 0.0, y: 100.0 }, IonPoint { x: 40.0, y: 60.0 }];
    let curve = route_via_attached(&via, &[], 4.0, Some(aims)).unwrap();
    assert_eq!(curve.first(), Some(&aims[0]));
    assert_eq!(curve.last(), Some(&aims[1]));
    assert!(curve.iter().all(|p| p.y >= 60.0));
    for i in (3..curve.len() - 1).step_by(3) {
        let a = (curve[i].x-curve[i-1].x, curve[i].y-curve[i-1].y);
        let b = (curve[i+1].x-curve[i].x, curve[i+1].y-curve[i].y);
        assert!(a.0*b.0+a.1*b.1 >= 0.0);
    }
}

#[test]
fn sparse_ranks_preserve_minlen_spacing_without_allocating_empty_rows() {
    use ion_layout::{
        compat::{layout, EdgeConstraint, NodeConstraint, Options},
        core::{NodeSpec, Orientation},
    };
    let nodes: Vec<_> = [20.0, 40.0, 30.0]
        .into_iter()
        .map(|height| NodeSpec {
            width: 30.0,
            height,
            loop_depth: 0,
            loop_header: false,
            backedge: false,
        })
        .collect();
    let nc = vec![
        NodeConstraint {
            group: usize::MAX,
            kind: 0,
            cluster: usize::MAX
        };
        3
    ];
    for minlen in [1, 4, u32::MAX] {
        let ec = vec![
            EdgeConstraint {
                minlen,
                constraint: 1,
                weight: 1.0,
                kind: 0,
                channel: 0
            };
            2
        ];
        for equally in [0, 1] {
            let result = layout(
                &nodes,
                &[(0, 1), (1, 2)],
                Orientation::TopToBottom,
                &nc,
                &ec,
                &mut [],
                Options {
                    nodesep: 10.0,
                    ranksep: 10.0,
                    ranksep_equally: equally,
                    arrow_clearance: 0.0,
                },
            );
            assert_eq!(
                result.node_layers,
                vec![0, minlen as usize, 2 * minlen as usize]
            );
            let first_step = result.positions[1].y - result.positions[0].y;
            let second_step = result.positions[2].y - result.positions[1].y;
            let expected_first = if equally == 1 {
                50.0 * minlen as f64
            } else {
                30.0 + 10.0 * minlen as f64
            };
            let expected_second = if equally == 1 {
                expected_first
            } else {
                35.0 + 10.0 * minlen as f64
            };
            assert!((first_step - expected_first).abs() < 1e-5);
            assert!((second_step - expected_second).abs() < 1e-5);
            assert!(result.height.is_finite());
        }
    }
}
