// cap.c.inc

static struct ObjectHitbox sCapHitbox = {
    /* interactType:      */ INTERACT_CAP,
    /* downOffset:        */ 0,
    /* damageOrCoinValue: */ 0,
    /* health:            */ 0,
    /* numLootCoins:      */ 0,
    /* radius:            */ 80,
    /* height:            */ 80,
    /* hurtboxRadius:     */ 90,
    /* hurtboxHeight:     */ 90,
};

static f32 kart_visual_hop_offset(void) {
    if (gMarioState->action == ACT_KART_DRIVING
        && (gMarioState->actionState == KART_STATE_DRIFT_LEFT
            || gMarioState->actionState == KART_STATE_DRIFT_RIGHT)
        && gMarioState->actionTimer < 10) {
        return sins((s16)(gMarioState->actionTimer * 0x0CCD)) * 45.0f;
    }

    return 0.0f;
}

static void kart_rotate_matrix_local_x(Mat4 mtx, s16 angle) {
    s32 i;
    f32 y;
    f32 z;
    f32 s = sins(angle);
    f32 c = coss(angle);

    for (i = 0; i < 3; i++) {
        y = mtx[1][i];
        z = mtx[2][i];
        mtx[1][i] = y * c + z * s;
        mtx[2][i] = z * c - y * s;
    }
}

static void kart_rotate_matrix_local_z(Mat4 mtx, s16 angle) {
    s32 i;
    f32 x;
    f32 y;
    f32 s = sins(angle);
    f32 c = coss(angle);

    for (i = 0; i < 3; i++) {
        x = mtx[0][i];
        y = mtx[1][i];
        mtx[0][i] = x * c + y * s;
        mtx[1][i] = y * c - x * s;
    }
}

static void kart_build_visual_matrix(struct Object *obj, s16 yaw, s16 wheelSpin, s16 bodyRoll) {
    Vec3f position;
    Vec3f upDir;

    position[0] = obj->oPosX;
    position[1] = obj->oPosY;
    position[2] = obj->oPosZ;

    if (gMarioState->action == ACT_KART_DRIVING && gMarioState->floor != NULL) {
        upDir[0] = gMarioState->floor->normal.x;
        upDir[1] = gMarioState->floor->normal.y;
        upDir[2] = gMarioState->floor->normal.z;
    } else {
        upDir[0] = 0.0f;
        upDir[1] = 1.0f;
        upDir[2] = 0.0f;
    }

    mtxf_align_terrain_normal(obj->transform, upDir, position, yaw);

    if (bodyRoll != 0) {
        kart_rotate_matrix_local_z(obj->transform, bodyRoll);
    }

    if (wheelSpin != 0) {
        kart_rotate_matrix_local_x(obj->transform, wheelSpin);
    }

    obj->header.gfx.throwMatrix = &obj->transform;
}

void bhv_kart_visual_loop(void) {
    struct Object *marioObj = o->parentObj;
    s16 yaw;
    s16 steerAngle = 0;
    s16 leanAngle = 0;
    s16 slipAngle;
    f32 steeringInput = 0.0f;
    f32 hop;
    f32 forwardOffset = 0.0f;
    f32 heightOffset = 0.0f;

    if (marioObj != gMarioObject || marioObj->activeFlags == ACTIVE_FLAG_DEACTIVATED
        || !(gMarioState->flags & MARIO_KART_HAT)) {
        o->activeFlags = ACTIVE_FLAG_DEACTIVATED;
        return;
    }

    yaw = gMarioState->faceAngle[1];
    hop = kart_visual_hop_offset();

    /*
     * Front/rear axle offsets come directly from the original MKDS wheel mesh
     * centres, multiplied by its Fast64 GEO_SCALE of 9.
     */
    if (o->oBehParams2ndByte == 1) {
        forwardOffset = 342.0f;
        heightOffset = 90.0f;
    } else if (o->oBehParams2ndByte == 2) {
        forwardOffset = -243.0f;
        heightOffset = 135.0f;
    }

    o->oPosX = gMarioState->pos[0] + forwardOffset * sins(yaw);
    o->oPosY = gMarioState->pos[1] + heightOffset + hop;
    o->oPosZ = gMarioState->pos[2] + forwardOffset * coss(yaw);

    if (o->oBehParams2ndByte == 0) {
        /*
         * Chassis roll follows slip angle, so normal driving stays nearly
         * level while a drift visibly loads the outside suspension.
         */
        slipAngle = (s16)(gMarioState->faceAngle[1] - gMarioState->slideYaw);
        leanAngle = (s16)(-slipAngle / 4);

        if (leanAngle > 0x0700) {
            leanAngle = 0x0700;
        } else if (leanAngle < -0x0700) {
            leanAngle = -0x0700;
        }

        kart_build_visual_matrix(o, yaw, 0, leanAngle);
    } else {
        /*
         * The tyre mesh rotates around its local X axis. Keep an accumulator
         * on the visual object itself so forward/reverse spin naturally in
         * opposite directions.
         */
        o->oFaceAnglePitch -= (s16)(gMarioState->forwardVel * 64.0f);

        if (o->oBehParams2ndByte == 1 && (gMarioState->input & INPUT_NONZERO_ANALOG)) {
            steeringInput =
                sins((s16)(gMarioState->intendedYaw - gMarioState->faceAngle[1]))
                * (gMarioState->intendedMag / 32.0f);

            if (steeringInput > 1.0f) {
                steeringInput = 1.0f;
            } else if (steeringInput < -1.0f) {
                steeringInput = -1.0f;
            }

            steerAngle = (s16)(steeringInput * 0x0900);
        }

        kart_build_visual_matrix(o, yaw + steerAngle, o->oFaceAnglePitch, 0);
    }
}

s32 cap_set_hitbox(void) {
    obj_set_hitbox(o, &sCapHitbox);
    if (o->oInteractStatus & INT_STATUS_INTERACTED) {
        o->activeFlags = ACTIVE_FLAG_DEACTIVATED;
        o->oInteractStatus = 0;
        return 1;
    }

    return 0;
}

void cap_despawn(void) {
    if (o->oTimer > 300) {
        obj_flicker_and_disappear(o, 300);
    }
}

void cap_check_quicksand(void) {
    if (sObjFloor == NULL)
        return;

    switch (sObjFloor->type) {
        case SURFACE_DEATH_PLANE:
            o->activeFlags = ACTIVE_FLAG_DEACTIVATED;
            break;

        case SURFACE_SHALLOW_QUICKSAND:
        case SURFACE_DEEP_QUICKSAND:
        case SURFACE_QUICKSAND:
            o->oAction = 10;
            o->oForwardVel = 0.0f;
            break;

        case SURFACE_DEEP_MOVING_QUICKSAND:
        case SURFACE_SHALLOW_MOVING_QUICKSAND:
        case SURFACE_MOVING_QUICKSAND:
            o->oAction = 11;
            o->oMoveAngleYaw = (sObjFloor->force & 0xFF) << 8;
            o->oForwardVel = 8 + 2 * (0 - ((sObjFloor->force & 0xFF00) >> 8));
            break;

        case SURFACE_INSTANT_QUICKSAND:
            o->oAction = 12;
            o->oForwardVel = 0.0f;
            break;

        case SURFACE_INSTANT_MOVING_QUICKSAND:
            o->oAction = 13;
            o->oMoveAngleYaw = (sObjFloor->force & 0xFF) << 8;
            o->oForwardVel = 8 + 2 * (0 - ((sObjFloor->force & 0xFF00) >> 8));
            break;
    }
}

void cap_sink_quicksand(void) {
    switch (o->oAction) {
        case 10:
            if (o->oTimer < 10) {
                o->oGraphYOffset += -1.0f;
                o->oFaceAnglePitch = 0x2000;
            }
            break;

        case 11:
            if (o->oTimer < 10)
                o->oGraphYOffset += -3.0f;

            o->oFaceAnglePitch = 0x2000;
            break;

        case 12:
            o->oGraphYOffset += -1.0f;
            if (o->oTimer >= 21)
                o->activeFlags = ACTIVE_FLAG_DEACTIVATED;

            break;

        case 13:
            o->oGraphYOffset += -6.0f;
            if (o->oTimer >= 21)
                o->activeFlags = ACTIVE_FLAG_DEACTIVATED;

            o->oFaceAnglePitch = 0x2000;
            break;
    }

    cap_check_quicksand();
}

void bhv_wing_cap_init(void) {
    o->oGravity = 1.2f;
    o->oFriction = 0.999f;
    o->oBuoyancy = 0.9f;
    o->oOpacity = 255;
}

void cap_scale_vertically(void) {
    o->oCapUnkF8 += 0x2000;
    o->header.gfx.scale[1] = coss(o->oCapUnkF8) * 0.3 + 0.7;
    if (o->oCapUnkF8 == 0x10000) {
        o->oCapUnkF8 = 0;
        o->oCapUnkF4 = 2;
    }
}

void wing_vanish_cap_act_0(void) {
    s16 sp1E;

    o->oFaceAngleYaw += o->oForwardVel * 128.0f;
    sp1E = object_step();
    if (sp1E & 0x01) {
        cap_check_quicksand();
        if (o->oVelY != 0.0f) {
            o->oCapUnkF4 = 1;
            o->oVelY = 0.0f;
        }
    }

    if (o->oCapUnkF4 == 1)
        cap_scale_vertically();
}

void bhv_wing_vanish_cap_loop(void) {
    switch (o->oAction) {
        case 0:
            wing_vanish_cap_act_0();
            break;

        default:
            object_step();
            cap_sink_quicksand();
            break;
    }

    if (o->oTimer > 20)
        cur_obj_become_tangible();

    cap_despawn();
    cap_set_hitbox();
}

void bhv_metal_cap_init(void) {
    o->oGravity = 2.4f;
    o->oFriction = 0.999f;
    o->oBuoyancy = 1.5f;
    o->oOpacity = 0xFF;
}

void metal_cap_act_0(void) {
    s16 sp1E;

    o->oFaceAngleYaw += o->oForwardVel * 128.0f;
    sp1E = object_step();
    if (sp1E & 0x01)
        cap_check_quicksand();
}

void bhv_metal_cap_loop(void) {
    switch (o->oAction) {
        case 0:
            metal_cap_act_0();
            break;

        default:
            object_step();
            cap_sink_quicksand();
            break;
    }

    if (o->oTimer > 20)
        cur_obj_become_tangible();

    cap_set_hitbox();
    cap_despawn();
}

void bhv_normal_cap_init(void) {
    o->oGravity = 0.7f;
    o->oFriction = 0.89f;
    o->oBuoyancy = 0.9f;
    o->oOpacity = 0xFF;

    save_file_set_cap_pos(o->oPosX, o->oPosY, o->oPosZ);
}

void normal_cap_set_save_flags(void) {
    save_file_clear_flags(SAVE_FLAG_CAP_ON_GROUND);

    switch (gCurrCourseNum) {
        case COURSE_SSL:
            save_file_set_flags(SAVE_FLAG_CAP_ON_KLEPTO);
            break;

        case COURSE_SL:
            save_file_set_flags(SAVE_FLAG_CAP_ON_MR_BLIZZARD);
            break;

        case COURSE_TTM:
            save_file_set_flags(SAVE_FLAG_CAP_ON_UKIKI);
            break;

        default:
            save_file_set_flags(SAVE_FLAG_CAP_ON_KLEPTO);
            break;
    }
}

void normal_cap_act_0(void) {
    s16 sp1E;

    o->oFaceAngleYaw += o->oForwardVel * 128.0f;
    o->oFaceAnglePitch += o->oForwardVel * 80.0f;
    sp1E = object_step();
    if (sp1E & 0x01) {
        cap_check_quicksand();

        if (o->oVelY != 0.0f) {
            o->oCapUnkF4 = 1;
            o->oVelY = 0.0f;
            o->oFaceAnglePitch = 0;
        }
    }

    if (o->oCapUnkF4 == 1)
        cap_scale_vertically();
}

void bhv_normal_cap_loop(void) {
    switch (o->oAction) {
        case 0:
            normal_cap_act_0();
            break;

        default:
            object_step();
            cap_sink_quicksand();
            break;
    }

    if ((s32) o->oForwardVel != 0)
        save_file_set_cap_pos(o->oPosX, o->oPosY, o->oPosZ);

    if (o->activeFlags == ACTIVE_FLAG_DEACTIVATED)
        normal_cap_set_save_flags();

    if (cap_set_hitbox() == 1)
        save_file_clear_flags(SAVE_FLAG_CAP_ON_GROUND);
}

void bhv_vanish_cap_init(void) {
    o->oGravity = 1.2f;
    o->oFriction = 0.999f;
    o->oBuoyancy = 0.9f;
    o->oOpacity = 150;
}
