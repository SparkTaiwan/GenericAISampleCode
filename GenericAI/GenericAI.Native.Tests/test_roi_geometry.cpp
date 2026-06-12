#include "CppUnitTest.h"

#include "roi_geometry.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GenericAINativeTests
{
    TEST_CLASS(RoiGeometryTest)
    {
    public:
        TEST_METHOD(BoxFullyInsideRoi_Overlaps)
        {
            DetectionRect b{ 10, 10, 20, 20 };
            ROIRect r{ 0, 0, 100, 100 };
            Assert::IsTrue(BoxOverlapsRoi(b, r));
        }

        TEST_METHOD(BoxFullyOutsideRoi_NoOverlap)
        {
            ROIRect r{ 50, 50, 100, 100 };
            // Left / right / above / below.
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 0, 60, 10, 10 }, r));
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 200, 60, 10, 10 }, r));
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 60, 0, 10, 10 }, r));
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 60, 200, 10, 10 }, r));
        }

        TEST_METHOD(BoxTouchingRoiEdge_NoOverlap)
        {
            ROIRect r{ 50, 50, 100, 100 };
            // Edge-to-edge contact is exclusive (<= / >= in the implementation).
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 40, 60, 10, 10 }, r));   // b right edge == r left edge
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 100, 60, 10, 10 }, r));  // b left edge == r right edge
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 60, 40, 10, 10 }, r));   // b bottom edge == r top edge
            Assert::IsFalse(BoxOverlapsRoi(DetectionRect{ 60, 100, 10, 10 }, r));  // b top edge == r bottom edge
        }

        TEST_METHOD(InvertedRoiCoordinates_SameResultAsNormalized)
        {
            DetectionRect inside{ 60, 60, 10, 10 };
            DetectionRect outside{ 0, 0, 10, 10 };
            ROIRect normal{ 50, 50, 100, 100 };
            ROIRect inverted{ 100, 100, 50, 50 };
            Assert::AreEqual(BoxOverlapsRoi(inside, normal), BoxOverlapsRoi(inside, inverted));
            Assert::AreEqual(BoxOverlapsRoi(outside, normal), BoxOverlapsRoi(outside, inverted));
            Assert::IsTrue(BoxOverlapsRoi(inside, inverted));
            Assert::IsFalse(BoxOverlapsRoi(outside, inverted));
        }

        TEST_METHOD(PartialOverlap_Overlaps)
        {
            DetectionRect b{ 40, 40, 20, 20 };   // spans 40..60, crosses ROI corner at 50,50
            ROIRect r{ 50, 50, 100, 100 };
            Assert::IsTrue(BoxOverlapsRoi(b, r));
        }

        TEST_METHOD(ScaleNotNeeded_WhenSpacesMatchOrUnknown)
        {
            Assert::IsFalse(RoiScaleNeeded(1920, 1080, 1920, 1080));
            Assert::IsFalse(RoiScaleNeeded(0, 0, 640, 360));       // legacy caller, no reference space
            Assert::IsFalse(RoiScaleNeeded(1920, 1080, 0, 0));     // degenerate frame
            Assert::IsTrue(RoiScaleNeeded(1920, 1080, 640, 360));
            Assert::IsTrue(RoiScaleNeeded(640, 360, 1920, 1080));
        }

        TEST_METHOD(ScaleRoiRect_DownscalesProportionally)
        {
            // 1920x1080 reference -> 640x360 frame: every coordinate divides by 3.
            ROIRect r{ 960, 540, 1920, 1080 };
            ScaleRoiRectToFrame(r, 1920, 1080, 640, 360);
            Assert::AreEqual(320, r.x1);
            Assert::AreEqual(180, r.y1);
            Assert::AreEqual(640, r.x2);
            Assert::AreEqual(360, r.y2);
        }

        TEST_METHOD(ScaleRoiRect_UpscalesProportionally)
        {
            ROIRect r{ 100, 50, 320, 180 };
            ScaleRoiRectToFrame(r, 640, 360, 1920, 1080);
            Assert::AreEqual(300, r.x1);
            Assert::AreEqual(150, r.y1);
            Assert::AreEqual(960, r.x2);
            Assert::AreEqual(540, r.y2);
        }

        TEST_METHOD(ScaleRoiRect_NoOpWhenReferenceUnknown)
        {
            ROIRect r{ 8, 8, 1912, 1072 };
            ScaleRoiRectToFrame(r, 0, 0, 640, 360);
            Assert::AreEqual(8, r.x1);
            Assert::AreEqual(8, r.y1);
            Assert::AreEqual(1912, r.x2);
            Assert::AreEqual(1072, r.y2);
        }

        TEST_METHOD(ScaleCoord_RoundsToNearest)
        {
            // 5 * 2/3 = 3.33 -> 3; 5 * 1/2 = 2.5 -> 3 (round half up).
            Assert::AreEqual(3, ScaleCoordToFrame(5, 3, 2));
            Assert::AreEqual(3, ScaleCoordToFrame(5, 2, 1));
        }
    };
}
