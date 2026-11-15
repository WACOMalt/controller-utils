#include "ui/body_pose.h"
#include <cassert>
#include <iostream>

int main() {
    ct::BodyPose pose;
    assert(!pose.fresh(.01));
    const std::string input =
        R"({"op":"pose","time":10,"position_time":10.05,"orientation_time":10.05,"position":[1,2,3],"orientation":[0.7071067811865476,0,0,0.7071067811865476]})";
    assert(ct::parse_command(input, pose) == "pose");
    auto point = ct::transform(pose.model, {1, 0, 0});
    assert(std::abs(point[0] - 1) < 1e-5 && std::abs(point[1] - 3) < 1e-5 &&
           std::abs(point[2] - 3) < 1e-5);
    assert(pose.fresh(10.06));
    assert(!pose.fresh(10.16) && !pose.fresh(10.01));
    assert(pose.fresh(10.8, 1.) && !pose.fresh(11.1, 1.));
    auto predicted = pose;
    predicted.position_time = predicted.orientation_time = 10.8;
    assert(predicted.fresh(10.81));
    predicted.position_time = 10.7;
    assert(!predicted.fresh(10.81));
    predicted.position_time = predicted.orientation_time = 610;
    assert(predicted.fresh(610.01));
    predicted.orientation_time = 609.9;
    assert(!predicted.fresh(610.01));
    assert(ct::parse_command(R"({"op":"hide"})", pose) == "hide");
    for (
        const auto &invalid :
        {input, input + "junk", std::string(4097, ' '), std::string("[]"),
         std::string(R"({"op":"quit","time":1})"),
         std::string(
             R"({"op":"pose","time":11,"position_time":11,"orientation_time":11,"position":[1,2,3],"orientation":[2,0,0,0]})"),
         std::string(
             R"({"op":"pose","time":11,"position_time":11,"orientation_time":11,"position":[true,2,3],"orientation":[1,0,0,0]})"),
         std::string(
             R"({"op":"pose","time":11,"position_time":11,"orientation_time":11,"position":[NaN,2,3],"orientation":[1,0,0,0]})"),
         std::string(
             R"({"op":"pose","time":12,"position_time":11,"orientation_time":11,"position":[1,2,3],"orientation":[1,0,0,0]})")}) {
        bool rejected = false;
        try {
            ct::parse_command(invalid, pose);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected && pose.optical_time == 10 && pose.orientation_time == 10.05);
    }
    ct::BodyPose native;
    assert(
        ct::parse_command(
            R"({"op":"native_pose","time":20,"position_time":20,"orientation_time":20,"position":[1,2,3],"orientation":[1,0,0,0]})",
            native) == "native_pose");
    assert(native.raw);
    auto origin = ct::identity();
    origin[12] = 4;
    assert(native.in_standing(origin).model[12] == 5 && !native.in_standing(origin).raw);
    origin[12] = -2;
    assert(native.in_standing(origin).model[12] == -1 && native.model[12] == 1);
    assert(pose.in_standing(origin).model == pose.model);
    std::cout << "body pose: raw origin changes, quaternion, freshness, ordering and malformed "
                 "input checks passed\n";
}
