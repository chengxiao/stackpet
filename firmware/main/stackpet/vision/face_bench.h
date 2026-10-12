/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace stackpet::vision {

/**
 * @brief Face detector benchmark, test builds only (`idf.py -DSTACKPET_FD_BENCH=1 build`).
 *
 * The RADAR app then starts this instead of the FaceDetector and keeps the head, the speaker and the LEDs idle; the
 * REMOTE app starts it next to the web page (its /snapshot.jpg shows the scene the logged face boxes refer to).
 * The bench runs every FaceModel variant (stack in PSRAM or internal RAM, internal RAM budgets for the feature maps,
 * candidate cap, 160x120 input, MSR threshold) on live RGB565 frames from the FrameGrabber, each in a fresh task, and
 * logs one "FaceBench" line per variant (average stage times, candidates, faces, heap). Returns at once.
 */
void startFaceBench();

}  // namespace stackpet::vision
