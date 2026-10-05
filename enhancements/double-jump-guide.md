# Adding a Mid-Air Double Jump

This guide adds one extra jump while Mario is already airborne. The implementation uses a new Mario action named `ACT_MIDAIR_DOUBLE_JUMP`.

The repository already has an `ACT_DOUBLE_JUMP`, but that action is the second jump in Mario's normal ground-based jump sequence. Do not reuse or replace it for this feature.

## Behavior

- Pressing A during `ACT_JUMP` starts `ACT_MIDAIR_DOUBLE_JUMP`.
- The new action resets Mario's vertical velocity and reuses the existing double-jump animation.
- A fresh `INPUT_A_PRESSED` edge is required, so holding A from the first jump does not trigger the second jump.
- The trigger exists only in `act_jump`, while the new action has its own handler. This limits Mario to one extra jump without adding a counter to `MarioState`.
- Diving and ground pounding keep their existing input priority.

## 1. Define the New Action

In `include/sm64.h`, add the new action to the airborne action group. Action ID `0x084` is unused in this group:

```c
#define ACT_JUMP                       0x03000880 // (0x080 | ACT_FLAG_AIR | ACT_FLAG_ALLOW_VERTICAL_WIND_ACTION | ACT_FLAG_CONTROL_JUMP_HEIGHT)
#define ACT_DOUBLE_JUMP                0x03000881 // (0x081 | ACT_FLAG_AIR | ACT_FLAG_ALLOW_VERTICAL_WIND_ACTION | ACT_FLAG_CONTROL_JUMP_HEIGHT)
#define ACT_TRIPLE_JUMP                0x01000882 // (0x082 | ACT_FLAG_AIR | ACT_FLAG_ALLOW_VERTICAL_WIND_ACTION)
#define ACT_BACKFLIP                   0x01000883 // (0x083 | ACT_FLAG_AIR | ACT_FLAG_ALLOW_VERTICAL_WIND_ACTION)
#define ACT_MIDAIR_DOUBLE_JUMP         0x03000884 // (0x084 | ACT_FLAG_AIR | ACT_FLAG_ALLOW_VERTICAL_WIND_ACTION | ACT_FLAG_CONTROL_JUMP_HEIGHT)
#define ACT_STEEP_JUMP                 0x03000885 // (0x085 | ACT_FLAG_AIR | ACT_FLAG_ALLOW_VERTICAL_WIND_ACTION | ACT_FLAG_CONTROL_JUMP_HEIGHT)
```

The flags match `ACT_JUMP`:

- `ACT_FLAG_AIR` routes the action through `mario_execute_airborne_action`.
- `ACT_FLAG_ALLOW_VERTICAL_WIND_ACTION` preserves vertical-wind behavior.
- `ACT_FLAG_CONTROL_JUMP_HEIGHT` lets releasing A shorten the second jump.

## 2. Initialize the Second Jump

In `src/game/mario.c`, add a case to `set_mario_action_airborne`. Place it next to the existing jump cases:

```c
case ACT_MIDAIR_DOUBLE_JUMP:
    set_mario_y_vel_based_on_fspeed(m, 52.0f, 0.25f);
    m->forwardVel *= 0.8f;
    break;
```

This runs once when `set_mario_action` enters the new action. The values match the vanilla `ACT_DOUBLE_JUMP`: a base vertical velocity of `52.0f`, a forward-speed contribution of `0.25f`, and a small horizontal-speed reduction.

For a weaker or stronger double jump, tune `52.0f`. Keep the velocity setup here instead of applying it every frame in the action handler.

## 3. Add the Action Handler and Trigger

In `src/game/mario_actions_airborne.c`, update `act_jump`. Add the A-button check after the existing Z-button check so dive and ground-pound inputs retain their current priority:

```c
s32 act_jump(struct MarioState *m) {
    if (check_kick_or_dive_in_air(m)) {
        return TRUE;
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    if (m->input & INPUT_A_PRESSED) {
        return set_mario_action(m, ACT_MIDAIR_DOUBLE_JUMP, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);
    common_air_action_step(m, ACT_JUMP_LAND, MARIO_ANIM_SINGLE_JUMP,
                           AIR_STEP_CHECK_LEDGE_GRAB | AIR_STEP_CHECK_HANG);
    return FALSE;
}
```

Then add the new handler near `act_double_jump`:

```c
s32 act_midair_double_jump(struct MarioState *m) {
    s32 animation = (m->vel[1] >= 0.0f)
        ? MARIO_ANIM_DOUBLE_JUMP_RISE
        : MARIO_ANIM_DOUBLE_JUMP_FALL;

    if (check_kick_or_dive_in_air(m)) {
        return TRUE;
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, SOUND_MARIO_HOOHOO);
    common_air_action_step(m, ACT_JUMP_LAND, animation,
                           AIR_STEP_CHECK_LEDGE_GRAB | AIR_STEP_CHECK_HANG);
    return FALSE;
}
```

There is deliberately no A-button transition in this handler. Once Mario enters `ACT_MIDAIR_DOUBLE_JUMP`, another A press cannot start a third jump.

The handler lands through `ACT_JUMP_LAND` rather than `ACT_DOUBLE_JUMP_LAND`. The latter belongs to the vanilla ground-jump sequence and can feed into triple-jump selection on the next takeoff.

## 4. Dispatch the New Action

In `mario_execute_airborne_action` in `src/game/mario_actions_airborne.c`, add the new case:

```c
case ACT_JUMP:                 cancel = act_jump(m);                 break;
case ACT_DOUBLE_JUMP:          cancel = act_double_jump(m);          break;
case ACT_MIDAIR_DOUBLE_JUMP:   cancel = act_midair_double_jump(m);   break;
case ACT_FREEFALL:             cancel = act_freefall(m);             break;
```

No declaration is required in `mario_actions_airborne.h` because the handler is used only within `mario_actions_airborne.c`.

## 5. Build and Test

Build the native port from the repository root:

```sh
make -j4 VERSION=us
```

Use the ROM version available in the repository if it is not `us`.

Test all of the following:

1. Tap A to jump, release it, then press A again in the air. Mario should gain height and use the double-jump animation.
2. Hold A continuously from takeoff. The second jump should not trigger until A is released and pressed again.
3. Press A repeatedly after the mid-air jump. Mario should not receive a third jump.
4. Press B or Z during either jump. Dive/kick and ground-pound behavior should still work.
5. Land after the second jump. Mario should return to normal ground actions without entering the vanilla triple-jump sequence unexpectedly.
6. Test a wall collision, ledge grab, ceiling grab, vertical wind, water entry, and fall damage to confirm the shared airborne handling still applies.

## Optional Eligibility Changes

As written, Mario can double jump only after starting with `ACT_JUMP`. This keeps the change narrow and makes the new action itself the consumed-jump state.

To allow a double jump after walking off a ledge, add the same `INPUT_A_PRESSED` transition to `act_freefall`. To allow it from backflips, side flips, long jumps, or other airborne actions, add the transition to each chosen handler. Do not add it to `act_midair_double_jump`, or repeated A presses will allow unlimited jumps.

If many starting actions should share the feature, a dedicated availability flag or counter in `MarioState` is easier to maintain than duplicating the input check. Reset that value on grounded takeoff, consume it when entering `ACT_MIDAIR_DOUBLE_JUMP`, and decide explicitly which special actions should replenish or preserve it.