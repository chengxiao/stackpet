/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
// Host tests of the yaw servo's wheel mode entry (stackchan/motion/servo.*): once rotate() switched to wheel mode,
// the position animation must not write a position again (that would switch the servo back to position mode and
// the head stops spinning; the 2026-10-12 device run where the head never turned).
#include "stackpet_games_check.h"
#include <hal/hal.h>
#include <stackchan/motion/servo.h>
#include <iostream>

using stackchan::motion::Servo;

namespace {

// The ScsServo's mode handling without the bus: any position write ends the wheel mode
class FakeServo : public Servo {
public:
    int writes = 0;
    bool wheel = false;

    FakeServo()
    {
        set_angle_limit({-1280, 1280});
    }

    void rotate(int) override
    {
        if (!wheel) {
            begin_rotation();  // what ScsServo::rotate() does on entering wheel mode
        }
        wheel = true;
    }
    void stopRotation() override
    {
        wheel = false;
    }
    void teleportForTest(int angle)
    {
        stop_motion_at_angle(angle);
    }

protected:
    void set_angle_impl(int) override
    {
        writes++;
        wheel = false;
    }
};

void run_ms(FakeServo& servo, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        GetHAL().now += 10;
        servo.update();
    }
}

// Move, and update until the animation is at rest, its snap to the target still to be written (as when the app
// starts spinning in the very tick the yaw spring came to rest)
void move_until_at_rest(FakeServo& servo, int angle)
{
    servo.setAutoTorqueReleaseEnabled(false);
    servo.move(angle);
    for (int i = 0; i < 1000 && servo.isMoving(); i++) {
        GetHAL().now += 20;
        servo.update();
    }
    CHECK(!servo.isMoving());
}

void test_rotate_after_a_move()
{
    FakeServo servo;
    move_until_at_rest(servo, 300);
    const int before = servo.writes;
    CHECK(before > 0);
    servo.rotate(-800);
    CHECK(servo.wheel);
    run_ms(servo, 500);
    CHECK(servo.writes == before);  // nothing ended the wheel mode
    CHECK(servo.wheel);
}

void test_teleport_would_end_the_wheel_mode()
{
    // Why begin_rotation() only drops the snap: teleporting restarts the animation, and its next step writes
    FakeServo servo;
    move_until_at_rest(servo, -200);
    servo.rotate(-800);
    servo.teleportForTest(-200);
    run_ms(servo, 100);
    CHECK(!servo.wheel);
}

void test_stop_then_move_writes_again()
{
    FakeServo servo;
    move_until_at_rest(servo, 100);
    servo.rotate(-800);
    run_ms(servo, 200);
    servo.stopRotation();
    const int before = servo.writes;
    servo.move(400);
    run_ms(servo, 2000);
    CHECK(servo.writes > before);  // position mode works as usual afterwards
}

}  // namespace

int main()
{
    RUN_TEST(test_rotate_after_a_move);
    RUN_TEST(test_teleport_would_end_the_wheel_mode);
    RUN_TEST(test_stop_then_move_writes_again);
    std::cout << "all radar servo tests passed (" << stackpet_test::checks() << " checks)\n";
    return 0;
}
