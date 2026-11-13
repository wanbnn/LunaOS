# LunaOS Motion Engine v2 — native UI transitions

The LunaOS compositor uses fixed-size, integer-only tween channels. The v2 presets are **native adaptations inspired by the motion patterns in [Transitions.dev](https://transitions.dev/)**; they do not embed its CSS/React catalog, execute npm in the guest, or provide general CSS animation support.

## Native desktop presets

| Native preset | Component | Easing | Duration |
| --- | --- | --- | --- |
| `MOTION_RECIPE_WINDOW_ENTER` | New native window | ease-out-back (subtle overshoot) | 220 ms |
| `MOTION_RECIPE_WINDOW_RESTORE` | Restore minimized window | ease-out-cubic | 200 ms |
| `MOTION_RECIPE_WINDOW_FOCUS` | Change focused window | ease-out-cubic | 140 ms |
| `MOTION_RECIPE_PANEL_REVEAL` | Control Center panel | ease-out-cubic | 180 ms |
| `MOTION_RECIPE_MENU_DROPDOWN` | App launcher | ease-out-cubic | 160 ms |

Channels 0 through APP_COUNT-1 are reserved for the native windows; two additional channels belong to the app launcher and Control Center. Panel animations change the **draw origin**, not the canonical control bounds. Pointer hit-testing subtracts the temporary draw offset, so controls remain clickable during opening.

`motion_begin()` remains backward-compatible (smoothstep Q10). The new `motion_begin_recipe()` selects an easing curve and duration for a native preset, using the 1 kHz monotonic kernel clock and integer arithmetic. This code does not allocate new heap memory, use floating-point operations, require a browser, or add a GPU-specific dependency.

The compositor invalidates both the previous and the new footprint while an animation runs. It does not repaint motion channels when they are idle. Dragging, maximizing, minimizing, closing and resolution switching cancel window animations.

## Current limitations

- Only the **five native presets above** are implemented. This is **not** a port of all the free CSS and React recipes installed by `npx transitions-dev add --free`.
- Panel opening is animated; closing is still immediate.
- Alpha transitions, real scale/blur/3D transforms, CSS keyframes, Web Animations API and React components are **not supported by these changes**. Integrating the CSS snippets into Luna Browser requires a separate browser engine feature and visual tests.
- A user-facing reduced-motion preference still needs implementation; this patch does not claim full accessibility parity.
- The 60 FPS target, real-device smoothness and ISO size must be measured in QEMU and on supported hardware; host unit tests are not sufficient.

## Testing

Run `make test-motion`, `make check`, then launch QEMU and exercise window open/restore/focus, launcher toggle and Control Center toggle. `tests/motion.c` checks wraparound, interpolation timing, cancellation, the easing endpoint and overshoot.

## License and upstream acknowledgement

Transitions.dev was created by Jakub Antalik: https://github.com/Jakubantalik/transitions.dev. Its terms permit using and adapting transitions in products, but prohibit redistribution of the collection as a competing transitions library. See the upstream [LICENSE](https://github.com/Jakubantalik/transitions.dev/blob/main/LICENSE) and [terms](https://transitions.dev/terms.html).

This change introduces **independent native C motion presets**; it does not vendor the transitions.dev catalog, website assets, or branding. The command `npx transitions-dev add --free` remains a **host-side** developer workflow for CSS/React projects rather than a LunaOS guest installation method.
