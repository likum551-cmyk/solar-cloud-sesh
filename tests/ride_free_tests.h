// 0.4.30 ride freedom (VehicleDriveConfig::rideTolM / tiltTolDeg, engine switch veh_ride_free): a copy rests on its own suspension.
// Included by vehicle_drive_test.cpp after its helpers (Check, V, Still, Input, vq).
//
// The defect (a friend's Enhanced 1158 log, 2026-11-05): GI's rs6 copy standing still, the ground-contact flag rising 22 times in 2 s, rotation
// jitter up to 2.2 deg and growing. A converted pack car's rest ride height / pitch differ between the machines; the full 3D correction pushed
// the copy's body toward the OWNER's rest pose every frame, against its own suspension. The model here: the game overwrites the body's velocity
// and angular velocity with the controller's command each frame, then its suspension (a damped spring to the copy's OWN rest height and pitch)
// acts for the rest of the frame. The measure is the suspension force left at steady state: the controller fighting the body.
#pragma once

namespace ridefree {

constexpr int kRideChecks = 11;   // the Check calls in TestRideFreedom

struct SuspResult {
    double zOff = 0, pitchOffDeg = 0;     // steady state: the body's offset from ITS OWN rest height / pitch
    double forceZ = 0, forceP = 0;        // |suspension acceleration| at steady state (m/s^2, rad/s^2): the fight
};

// A copy whose rest pose is dz below and dPitchDeg less nose-up than the owner's report, both standing still, 10 s at 30 fps.
static SuspResult RunSuspension(float tolM, float tiltDeg, double dz, double dPitchDeg) {
    VehicleDriveConfig cfg;
    cfg.contactKickMs = 0.0f;                 // the spring moves the body away from the command every frame: not a crash
    cfg.contactGraceMs = 0.0f;
    cfg.rideTolM = tolM;
    cfg.tiltTolDeg = tiltDeg;
    VehicleDriveController c(cfg);
    const double kPiL = 3.14159265358979323846;
    const double zRest = 0.0, pRest = 0.0;                       // the copy's own rest pose
    const double zT = zRest + dz, pT = pRest + dPitchDeg * kPiL / 180.0;   // the owner's report
    const double k = 60.0, damp = 4.0, kp = 60.0, dampP = 4.0;  // ~1.2 Hz body modes, as a car's
    const double dtMs = 1000.0 / 30.0, dt = dtMs / 1000.0;
    const VehicleSample tgt = Still(V(0, 0, zT), vq::Exp(V(pT, 0, 0)));
    double z = zRest, vz = 0, p = pRest, wp = 0;
    c.Update(Input(tgt, V(0, 0, z), vq::Exp(V(p, 0, 0)), V(0, 0, vz), dtMs));   // first frame: a snap onto the owner's pose (as spawn does)
    z = zT; p = pT;
    SuspResult r;
    for (int i = 0; i < 300; ++i) {
        const VehicleDriveOutput o = c.Update(Input(tgt, V(0, 0, z), vq::Exp(V(p, 0, 0)), V(0, 0, vz), dtMs));
        if (o.setVelocity) vz = o.velocity.z;
        if (o.setAngularVelocity) wp = o.angularVelocity.x;
        const double az = -k * (z - zRest) - damp * vz, ap = -kp * (p - pRest) - dampP * wp;
        vz += az * dt; z += vz * dt;
        wp += ap * dt; p += wp * dt;
        r.forceZ = std::fabs(k * (z - zRest));
        r.forceP = std::fabs(kp * (p - pRest));
    }
    r.zOff = z - zRest;
    r.pitchOffDeg = (p - pRest) * 180.0 / kPiL;
    return r;
}

static void TestRideFreedom() {
    std::printf("-- 0.4.30 ride freedom: a copy rests on its own suspension\n");
    char got[256];
    // the defect first: the 0.4.29 controller (tolerances 0) holds the copy off its own rest pose, the suspension pushing back for ever
    const SuspResult old = RunSuspension(0.0f, 0.0f, 0.10, 3.0);
    std::snprintf(got, sizeof(got), "z off %.3f m (force %.2f m/s^2), pitch off %.2f deg (force %.2f rad/s^2)", old.zOff, old.forceZ, old.pitchOffDeg, old.forceP);
    Check("0.4.29 control: a copy 10 cm / 3 deg off the owner's rest pose is held off its own (the fight)", old.zOff > 0.05 && old.pitchOffDeg > 1.5, got);
    // the fix: inside the tolerances the copy settles on its own rest pose and the suspension force dies out
    const SuspResult fix = RunSuspension(kRideTolM, kTiltTolDeg, 0.10, 3.0);
    std::snprintf(got, sizeof(got), "z off %.4f m (force %.3f m/s^2), pitch off %.3f deg (force %.3f rad/s^2)", fix.zOff, fix.forceZ, fix.pitchOffDeg, fix.forceP);
    Check("ride freedom: the copy rests on its own height and pitch, no force left", std::fabs(fix.zOff) < 0.005 && std::fabs(fix.pitchOffDeg) < 0.1 &&
          fix.forceZ < 0.3 && fix.forceP < 0.1, got);
    // beyond the tolerance only the excess is corrected: the owner's report 0.35 m up (under the 0.4 m under-floor snap) pulls the copy
    // about the excess (0.10 m) against its spring, not the whole 0.35 (0.4.29 would hold it ~0.29 up)
    const SuspResult up35 = RunSuspension(kRideTolM, kTiltTolDeg, 0.35, 0.0);
    const SuspResult up35old = RunSuspension(0.0f, 0.0f, 0.35, 0.0);
    std::snprintf(got, sizeof(got), "z off %.3f m (0.4.29: %.3f m; the owner's report is 0.35 m up)", up35.zOff, up35old.zOff);
    Check("ride freedom: beyond the band the excess is still corrected, only the excess", up35.zOff > 0.03 && up35.zOff < 0.11 && up35old.zOff > 0.2, got);

    // unit rules on one frame
    {
        VehicleDriveConfig cfg;
        cfg.rideTolM = kRideTolM;
        cfg.tiltTolDeg = kTiltTolDeg;
        const double dtMs = 1000.0 / 30.0;
        auto vzFor = [&](double dz) {
            VehicleDriveController c(cfg);
            const VehicleSample t = Still(V(0, 0, dz), vq::FromHeadingDeg(0));
            c.Update(Input(t, V(0, 0, dz), vq::FromHeadingDeg(0), V(0, 0, 0), dtMs));   // snap
            return c.Update(Input(t, V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), dtMs)).velocity.z;
        };
        const float a = vzFor(0.20), b = vzFor(0.30), d = vzFor(0.40), n = vzFor(-0.40);
        std::snprintf(got, sizeof(got), "vz for 0.20 / 0.30 / 0.40 / -0.40 m: %.3f / %.3f / %.3f / %.3f", a, b, d, n);
        Check("vertical: nothing inside the band, the excess beyond it (small at the edge), both signs",
              a == 0.0f && b > 0.0f && b < 0.6f && d > 2.0f * b && n < 0.0f && std::fabs(n + d) < 1e-4f, got);

        // a yaw error is corrected as before, a pitch error inside the band is not
        auto wFor = [&](const DriveQuat& target) {
            VehicleDriveController c(cfg);
            const VehicleSample t = Still(V(0, 0, 0), target);
            c.Update(Input(t, V(0, 0, 0), target, V(0, 0, 0), dtMs));
            return c.Update(Input(t, V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), dtMs)).angularVelocity;
        };
        const DriveVec yaw = wFor(vq::FromHeadingDeg(5)), pitch = wFor(vq::Exp(V(4.0 * 3.14159265358979 / 180.0, 0, 0)));
        std::snprintf(got, sizeof(got), "5 deg yaw -> w (%.3f %.3f %.3f); 4 deg pitch -> w (%.3f %.3f %.3f)", yaw.x, yaw.y, yaw.z, pitch.x, pitch.y, pitch.z);
        Check("rotation: a 5 deg yaw error is corrected, a 4 deg pitch error is left to the suspension",
              std::fabs(yaw.z) > 0.5f && std::fabs(pitch.x) < 1e-5f && std::fabs(pitch.y) < 1e-5f, got);

        // off (0.4.29): the same pitch error is corrected
        VehicleDriveController c0;
        const DriveQuat pq = vq::Exp(V(4.0 * 3.14159265358979 / 180.0, 0, 0));
        c0.Update(Input(Still(V(0, 0, 0), pq), V(0, 0, 0), pq, V(0, 0, 0), dtMs));
        const DriveVec w0 = c0.Update(Input(Still(V(0, 0, 0), pq), V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), dtMs)).angularVelocity;
        std::snprintf(got, sizeof(got), "w (%.3f %.3f %.3f)", w0.x, w0.y, w0.z);
        Check("off (tolerances 0): the pitch error is corrected as in 0.4.29", std::fabs(w0.x) > 0.3f, got);

        const double d2r = 3.14159265358979 / 180.0;
        // the rotation command for a copy at `local` against `target` (velocity vzT), second frame (the first snaps)
        auto wAt = [&](const DriveQuat& local, const DriveQuat& target, double vzT) {
            VehicleDriveController c(cfg);
            VehicleSample t = Still(V(0, 0, 0), target);
            t.vel = V(0, 0, vzT);
            c.Update(Input(t, V(0, 0, 0), target, V(0, 0, vzT), dtMs));
            return c.Update(Input(t, V(0, 0, 0), local, V(0, 0, vzT), dtMs)).angularVelocity;
        };
        // on a 15 deg slope a 3 deg roll difference is ALL tilt (its axis lies in the body's plane): no command at all, yaw included
        // (the world split put sin(15) of it, 0.78 deg, into "yaw" and kept correcting it)
        const DriveQuat slope = vq::Exp(V(15 * d2r, 0, 0));
        const DriveVec ws = wAt(slope, vq::Mul(slope, vq::Exp(V(0, 3 * d2r, 0))), 0.0);
        std::snprintf(got, sizeof(got), "w (%.5f %.5f %.5f)", ws.x, ws.y, ws.z);
        Check("slope: a roll difference on a 15 deg hill is left alone on every axis (tilt about the body's own up)",
              std::fabs(ws.x) < 1e-4f && std::fabs(ws.y) < 1e-4f && std::fabs(ws.z) < 1e-4f, got);
        // a car rolled 80 deg onto its side: its heading lies in the world-horizontal plane; the band is off there, a 5 deg yaw is corrected
        const DriveQuat side = vq::Exp(V(0, 80 * d2r, 0));
        const DriveVec wy = wAt(side, vq::Mul(side, vq::Exp(V(0, 0, 5 * d2r))), 0.0);
        const float wyL = (float)std::sqrt((double)wy.x * wy.x + (double)wy.y * wy.y + (double)wy.z * wy.z);
        std::snprintf(got, sizeof(got), "|w| %.3f (%.3f %.3f %.3f)", wyL, wy.x, wy.y, wy.z);
        Check("on its side: a 5 deg heading error is corrected (no band past upright)", wyL > 0.5f, got);
        // airborne: the bands fade with the vertical speed (full under 1.5 m/s, half at 2.0, none at 2.5 and over)
        const DriveQuat p4 = vq::Exp(V(4 * d2r, 0, 0));
        const DriveVec wJump = wAt(vq::FromHeadingDeg(0), p4, 3.0), wHalf = wAt(vq::FromHeadingDeg(0), p4, 2.0);
        std::snprintf(got, sizeof(got), "4 deg pitch at vz 3.0: w.x %.3f; at vz 2.0: w.x %.3f", wJump.x, wHalf.x);
        Check("airborne: a jump gets the full correction, the band fades in between (no step)",
              std::fabs(wJump.x) > 0.5f && std::fabs(wHalf.x) > 0.05f && std::fabs(wHalf.x) < 0.3f, got);
        // inside a contact grace (weight past a half) the same band applies: a 4 deg pitch error is not corrected
        {
            VehicleDriveConfig gc = cfg;
            gc.contactKickMs = 0.0f;
            VehicleDriveController c(gc);
            const VehicleSample t = Still(V(0, 0, 0), p4);
            c.Update(Input(t, V(0, 0, 0), p4, V(0, 0, 0), dtMs));
            VehicleDriveInput hit = Input(t, V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), dtMs);
            hit.contact = true;
            const VehicleDriveOutput o1 = c.Update(hit);
            VehicleDriveOutput o = o1;
            for (int i = 0; i < 9; ++i) o = c.Update(Input(t, V(0, 0, 0), vq::FromHeadingDeg(0), V(0, 0, 0), dtMs));
            std::snprintf(got, sizeof(got), "grace opened %d, active %d weight %.2f, angular set %d, w.x %.4f", (int)o1.contactOpened, (int)o.graceActive,
                          o.graceWeight, (int)o.setAngularVelocity, o.angularVelocity.x);
            Check("grace: with the weight past a half, the pitch band holds there too", o1.contactOpened != 0 && o.graceActive && o.graceWeight >= 0.5f &&
                  o.setAngularVelocity && std::fabs(o.angularVelocity.x) < 1e-5f, got);
        }
    }
    {
        const bool ok = RideFreeClass(0) && RideFreeClass(7) && RideFreeClass(9) && RideFreeClass(22) && !RideFreeClass(8) && !RideFreeClass(13) &&
                        !RideFreeClass(14) && !RideFreeClass(15) && !RideFreeClass(16) && !RideFreeClass(21) && !RideFreeClass(-1) && !RideFreeClass(-2);
        Check("classes: cars, off-road, open wheel yes; bikes, cycles, boats, helis, planes, trains, unknown no", ok);
    }
}

}   // namespace ridefree
