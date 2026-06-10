#include "CppUnitTest.h"

#include "yolox_geometry.h"

#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GenericAINativeTests
{
    TEST_CLASS(IouTest)
    {
    public:
        TEST_METHOD(IdenticalBoxes_IouIsOne)
        {
            DetectionRect a{ 10, 10, 50, 50 };
            Assert::AreEqual(1.0f, Iou(a, a), 1e-6f);
        }

        TEST_METHOD(DisjointBoxes_IouIsZero)
        {
            Assert::AreEqual(0.0f, Iou(DetectionRect{ 0, 0, 10, 10 },
                                       DetectionRect{ 100, 100, 10, 10 }), 1e-6f);
        }

        TEST_METHOD(ContainedBox_IouIsAreaRatio)
        {
            DetectionRect outer{ 0, 0, 10, 10 };   // area 100
            DetectionRect inner{ 2, 2, 5, 5 };     // area 25, fully inside
            Assert::AreEqual(0.25f, Iou(outer, inner), 1e-6f);
            Assert::AreEqual(0.25f, Iou(inner, outer), 1e-6f);
        }

        TEST_METHOD(DegenerateBox_IouIsZero)
        {
            DetectionRect full{ 0, 0, 10, 10 };
            Assert::AreEqual(0.0f, Iou(DetectionRect{ 0, 0, 0, 10 }, full), 1e-6f);
            Assert::AreEqual(0.0f, Iou(DetectionRect{ 0, 0, 10, 0 }, full), 1e-6f);
            // Both degenerate: union is 0, the uni > 0 guard must kick in.
            Assert::AreEqual(0.0f, Iou(DetectionRect{ 0, 0, 0, 0 },
                                       DetectionRect{ 0, 0, 0, 0 }), 1e-6f);
        }

        TEST_METHOD(EdgeTouchingBoxes_IouIsZero)
        {
            Assert::AreEqual(0.0f, Iou(DetectionRect{ 0, 0, 10, 10 },
                                       DetectionRect{ 10, 0, 10, 10 }), 1e-6f);
        }
    };

    TEST_CLASS(NmsTest)
    {
    public:
        TEST_METHOD(EmptyInput_KeepsNothing)
        {
            std::vector<ScoredBox> boxes;
            std::vector<DetectionRect> kept;
            Nms(boxes, 0.5f, kept);
            Assert::IsTrue(kept.empty());
        }

        TEST_METHOD(IdenticalBoxesSameScore_KeepsOne)
        {
            DetectionRect box{ 10, 10, 50, 50 };
            std::vector<ScoredBox> boxes{ { box, 0.9f }, { box, 0.9f }, { box, 0.9f } };
            std::vector<DetectionRect> kept;
            Nms(boxes, 0.5f, kept);
            Assert::AreEqual(1, static_cast<int>(kept.size()));
            Assert::AreEqual(box.x, kept[0].x);
        }

        TEST_METHOD(DisjointBoxes_AllKeptHighestScoreFirst)
        {
            std::vector<ScoredBox> boxes{
                { DetectionRect{ 0, 0, 10, 10 },  0.3f },
                { DetectionRect{ 20, 0, 10, 10 }, 0.9f },
                { DetectionRect{ 40, 0, 10, 10 }, 0.6f },
            };
            std::vector<DetectionRect> kept;
            Nms(boxes, 0.5f, kept);
            Assert::AreEqual(3, static_cast<int>(kept.size()));
            Assert::AreEqual(20, kept[0].x);  // score 0.9
            Assert::AreEqual(40, kept[1].x);  // score 0.6
            Assert::AreEqual(0, kept[2].x);   // score 0.3
        }

        TEST_METHOD(OverlapBelowThreshold_NotSuppressed)
        {
            // Half-width shift: inter 50, union 150, IoU = 1/3 < 0.5.
            std::vector<ScoredBox> boxes{
                { DetectionRect{ 0, 0, 10, 10 }, 0.9f },
                { DetectionRect{ 5, 0, 10, 10 }, 0.8f },
            };
            std::vector<DetectionRect> kept;
            Nms(boxes, 0.5f, kept);
            Assert::AreEqual(2, static_cast<int>(kept.size()));
        }
    };

    TEST_CLASS(ComputeLetterboxTest)
    {
    public:
        TEST_METHOD(SquareIntoSquare_NoScaleNoPad)
        {
            Letterbox lb = ComputeLetterbox(640, 640, 640, 640);
            Assert::AreEqual(1.0f, lb.scale, 1e-6f);
            Assert::AreEqual(640, lb.new_w);
            Assert::AreEqual(640, lb.new_h);
            Assert::AreEqual(0, lb.pad_x);
            Assert::AreEqual(0, lb.pad_y);
        }

        TEST_METHOD(WideSource_PadsVertically)
        {
            Letterbox lb = ComputeLetterbox(1920, 1080, 640, 640);
            Assert::AreEqual(640.0f / 1920.0f, lb.scale, 1e-6f);
            Assert::AreEqual(640, lb.new_w);
            Assert::AreEqual(360, lb.new_h);
            Assert::AreEqual(0, lb.pad_x);
            Assert::AreEqual(140, lb.pad_y);
        }

        TEST_METHOD(TallSource_PadsHorizontally)
        {
            Letterbox lb = ComputeLetterbox(1080, 1920, 640, 640);
            Assert::AreEqual(640.0f / 1920.0f, lb.scale, 1e-6f);
            Assert::AreEqual(360, lb.new_w);
            Assert::AreEqual(640, lb.new_h);
            Assert::AreEqual(140, lb.pad_x);
            Assert::AreEqual(0, lb.pad_y);
        }

        TEST_METHOD(OddPadding_TruncatesTowardZero)
        {
            // 640x479 into 640x640: scale 1.0, vertical slack 161 -> pad 80 (161/2).
            Letterbox lb = ComputeLetterbox(640, 479, 640, 640);
            Assert::AreEqual(1.0f, lb.scale, 1e-6f);
            Assert::AreEqual(479, lb.new_h);
            Assert::AreEqual(80, lb.pad_y);
            Assert::AreEqual(0, lb.pad_x);
        }
    };

    TEST_CLASS(GenerateGridStridesTest)
    {
    public:
        TEST_METHOD(Input640_Produces8400Grids)
        {
            std::vector<GridStride> out;
            GenerateGridStrides(640, 640, out);
            Assert::AreEqual(80 * 80 + 40 * 40 + 20 * 20, static_cast<int>(out.size()));
        }

        TEST_METHOD(Input416_Produces3549Grids)
        {
            std::vector<GridStride> out;
            GenerateGridStrides(416, 416, out);
            Assert::AreEqual(52 * 52 + 26 * 26 + 13 * 13, static_cast<int>(out.size()));
        }

        TEST_METHOD(StrideSegmentsOrderedAndRowMajor)
        {
            std::vector<GridStride> out;
            GenerateGridStrides(640, 640, out);
            // First entry of the stride-8 segment.
            Assert::AreEqual(0, out[0].grid_x);
            Assert::AreEqual(0, out[0].grid_y);
            Assert::AreEqual(8, out[0].stride);
            // Last entry of the stride-8 segment.
            const GridStride& last8 = out[80 * 80 - 1];
            Assert::AreEqual(79, last8.grid_x);
            Assert::AreEqual(79, last8.grid_y);
            Assert::AreEqual(8, last8.stride);
            // First entry of the stride-16 segment, last entry overall (stride 32).
            Assert::AreEqual(16, out[80 * 80].stride);
            const GridStride& last = out.back();
            Assert::AreEqual(19, last.grid_x);
            Assert::AreEqual(19, last.grid_y);
            Assert::AreEqual(32, last.stride);
        }

        TEST_METHOD(ReusedOutputVector_IsCleared)
        {
            std::vector<GridStride> out(7, GridStride{ 1, 2, 3 });
            GenerateGridStrides(640, 640, out);
            Assert::AreEqual(8400, static_cast<int>(out.size()));
            Assert::AreEqual(8, out[0].stride);
        }
    };
}
